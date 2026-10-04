/* PD-0 learner implementation. See pd0_learner.h. No generator knowledge,
 * no file I/O, no level identity. */
#include "pd0_learner.h"
#include "pd0_rng.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t before[PD0_MAX_OBS], after[PD0_MAX_OBS]; uint8_t chan; int64_t value; uint8_t tag; uint32_t episode; uint8_t is_reset; } trans;

struct pd0_learner {
    pd0l_desc d;
    uint64_t seed;
    trans *t; uint32_t nt;
    unsigned ni, nl;                                   /* inputs (obs + channels), library size */
    uint8_t lib[PD0L_MAX_LIB][PD0_MAX_VARS + PD0_MAX_CHAN];
    int ncand; pd0l_candidate cand[PD0L_MAX_CAND];
};

/* ---- vocabulary: every monomial of total degree <= 3 over the inputs (constant included) ---- */
static unsigned build_library(unsigned ni, uint8_t lib[PD0L_MAX_LIB][PD0_MAX_VARS + PD0_MAX_CHAN])
{
    unsigned n = 0; uint8_t e[PD0_MAX_VARS + PD0_MAX_CHAN];
    for (unsigned a = 0; a <= ni; a++) for (unsigned b = a; b <= ni; b++) for (unsigned c = b; c <= ni; c++) {
        memset(e, 0, sizeof e); if (a < ni) e[a]++; if (b < ni) e[b]++; if (c < ni) e[c]++;
        int dup = 0; for (unsigned k = 0; k < n; k++) if (!memcmp(lib[k], e, ni)) { dup = 1; break; }
        if (!dup && n < PD0L_MAX_LIB) memcpy(lib[n++], e, sizeof e); }
    return n;
}

pd0_learner *pd0_learner_new(const pd0l_desc *d, uint64_t seed)
{
    if (!d || d->n_obs == 0 || d->n_obs > PD0_MAX_OBS || d->n_channels > PD0_MAX_CHAN) return NULL;
    pd0_learner *L = calloc(1, sizeof *L); if (!L) return NULL;
    L->t = malloc(sizeof(trans) * PD0L_MAX_TRANS); if (!L->t) { free(L); return NULL; }
    L->d = *d; L->seed = seed; L->ni = d->n_obs + d->n_channels; L->nl = build_library(L->ni, L->lib);
    return L;
}
void pd0_learner_free(pd0_learner *L) { if (L) { free(L->t); free(L); } }

int pd0_learner_observe(pd0_learner *L, const pd0_rec *r, uint8_t tag)
{
    if (tag != TAG_FIT && tag != TAG_SELECT) return -1;              /* I10: nothing else may reach the learner */
    if (r->n_obs != L->d.n_obs || L->nt >= PD0L_MAX_TRANS) return -1;
    if (r->kind == PD0_KIND_STEP && r->status != PD0_ST_OK) return 0;  /* refused / ended steps carry no transition */
    trans *t = &L->t[L->nt++]; memset(t, 0, sizeof *t);
    memcpy(t->before, r->before, sizeof(int64_t) * r->n_obs); memcpy(t->after, r->after, sizeof(int64_t) * r->n_obs);
    t->chan = r->channel; t->value = r->applied; t->tag = tag; t->episode = r->episode; t->is_reset = r->kind == PD0_KIND_RESET;
    return 0;
}
uint32_t pd0_learner_n_transitions(const pd0_learner *L, uint8_t tag) { uint32_t n = 0; for (uint32_t i = 0; i < L->nt; i++) if (L->t[i].tag == tag && !L->t[i].is_reset) n++; return n; }

/* ---- schedules ---- */
static int64_t draw_in(pd0_rng *g, int64_t lo, int64_t hi) { int64_t u = pd0_rng_unit(g); return lo + pd0_mul(hi - lo, u); }
void pd0_learner_random_schedule(const pd0l_desc *d, uint64_t seed, const char *tag, uint32_t idx, int64_t *reset, pd0_step *steps, uint32_t n)
{
    pd0_rng g; pd0_rng_stream(&g, seed ^ ((uint64_t)idx * 0x9E3779B97F4A7C15ull), tag);
    for (int j = 0; j < d->n_obs; j++) reset[j] = draw_in(&g, d->reset_min[j], d->reset_max[j]);
    for (uint32_t s = 0; s < n; s++) {
        uint8_t ch = d->n_channels ? (uint8_t)(pd0_rng_next(&g) % d->n_channels) : PD0_CHAN_NONE;
        steps[s].channel = ch; steps[s].value = ch == PD0_CHAN_NONE ? 0 : draw_in(&g, d->chan_min[ch], d->chan_max[ch]); }
}
void pd0_learner_explore(pd0_learner *L, uint32_t episode_index, int64_t *reset, pd0_step *steps, uint32_t n_steps)
{ pd0_learner_random_schedule(&L->d, L->seed, "explore", episode_index, reset, steps, n_steps); }

/* ---- prediction over a relation (integer evaluator shared with the checker: pd0_rel_step) ---- */
void pd0_learner_rollout(const pd0_rel *rel, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out)
{
    int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; uint8_t no = (uint8_t)(rel->n_vars - rel->n_latent);
    memset(st, 0, sizeof st); for (int j = 0; j < no; j++) st[j] = init[j];
    for (uint32_t s = 0; s < n; s++) { pd0_rel_step(rel, st, steps[s].channel, steps[s].value, nx); for (int j = 0; j < no; j++) out[s * no + j] = nx[j]; memcpy(st, nx, sizeof st); }
}

typedef struct { double se[PD0_MAX_OBS], s[PD0_MAX_OBS], ss[PD0_MAX_OBS]; uint32_t n; } acc;
static void acc_add(acc *a, const int64_t *p, const int64_t *t, uint8_t no) { for (int j = 0; j < no; j++) { double e = (double)(p[j] - t[j]) / 1e6, v = (double)t[j] / 1e6; a->se[j] += e * e; a->s[j] += v; a->ss[j] += v * v; } a->n++; }
static int64_t acc_nrmse(const acc *a, uint8_t no)
{
    double w = 0; if (!a->n) return 0;
    for (int j = 0; j < no; j++) { double rmse = sqrt(a->se[j] / a->n), m = a->s[j] / a->n, var = a->ss[j] / a->n - m * m, sd = var > 0 ? sqrt(var) : 0, v = sd > 1e-9 ? rmse / sd : rmse; if (v > w) w = v; }
    return w > 9e12 ? INT64_MAX : (int64_t)(w * 1e6);
}
/* one-step NRMSE on the records of one tag; latent carried along each episode (as the checker's T3 does) */
static int64_t onestep_eval(const pd0_learner *L, const pd0_rel *rel, uint8_t tag, double *lag1)
{
    uint8_t no = L->d.n_obs; acc a; memset(&a, 0, sizeof a); int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; memset(st, 0, sizeof st);
    double num[PD0_MAX_OBS] = { 0 }, den[PD0_MAX_OBS] = { 0 }, prev[PD0_MAX_OBS] = { 0 }; int have_prev = 0;
    for (uint32_t i = 0; i < L->nt; i++) { const trans *t = &L->t[i]; if (t->tag != tag) continue;
        if (t->is_reset) { memset(st, 0, sizeof st); have_prev = 0; continue; }
        for (int v = 0; v < no; v++) st[v] = t->before[v];
        pd0_rel_step(rel, st, t->chan, t->value, nx); acc_add(&a, nx, t->after, no);
        for (int v = 0; v < no; v++) { double res = (double)(t->after[v] - nx[v]) / 1e6; if (have_prev) num[v] += res * prev[v]; den[v] += res * res; prev[v] = res; }
        have_prev = 1; memcpy(st, nx, sizeof st); }
    if (lag1) for (int v = 0; v < no; v++) lag1[v] = den[v] > 0 ? num[v] / den[v] : 0;
    return acc_nrmse(&a, no);
}
int64_t pd0_learner_onestep_nrmse(const pd0_learner *L, const pd0_rel *rel, uint8_t tag) { return onestep_eval(L, rel, tag, NULL); }

/* ---- least squares machinery ---- */
static double mono(const double *x, const uint8_t *e, unsigned ni) { double v = 1; for (unsigned i = 0; i < ni; i++) for (uint8_t k = 0; k < e[i]; k++) v *= x[i]; return v; }
static int solve(double *A, double *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) { unsigned p = i; for (unsigned r = i + 1; r < n; r++) if (fabs(A[r * n + i]) > fabs(A[p * n + i])) p = r;
        if (fabs(A[p * n + i]) < 1e-14) return -1;
        if (p != i) { for (unsigned k = 0; k < n; k++) { double t = A[i * n + k]; A[i * n + k] = A[p * n + k]; A[p * n + k] = t; } double t = b[i]; b[i] = b[p]; b[p] = t; }
        for (unsigned r = i + 1; r < n; r++) { double f = A[r * n + i] / A[i * n + i]; for (unsigned k = i; k < n; k++) A[r * n + k] -= f * A[i * n + k]; b[r] -= f * b[i]; } }
    for (int i = (int)n - 1; i >= 0; i--) { double s = b[i]; for (unsigned k = (unsigned)i + 1; k < n; k++) s -= A[i * n + k] * b[k]; b[i] = s / A[i * n + i]; }
    return 0;
}
typedef struct { const double *X; const double *y; uint32_t n; unsigned nl; } design;
/* fit the subset sel[0..m) by normal equations; returns RSS (INFINITY if singular) and coef */
static double fit_subset(const design *D, const unsigned *sel, unsigned m, double *coef)
{
    double A[PD0L_MAX_SEL * PD0L_MAX_SEL] = { 0 }, b[PD0L_MAX_SEL] = { 0 };
    for (uint32_t i = 0; i < D->n; i++) for (unsigned p = 0; p < m; p++) { double xp = D->X[i * D->nl + sel[p]]; b[p] += xp * D->y[i]; for (unsigned q = 0; q < m; q++) A[p * m + q] += xp * D->X[i * D->nl + sel[q]]; }
    for (unsigned p = 0; p < m; p++) A[p * m + p] += 1e-12;
    if (m && solve(A, b, m)) return INFINITY;
    double rss = 0; for (uint32_t i = 0; i < D->n; i++) { double pr = 0; for (unsigned p = 0; p < m; p++) pr += b[p] * D->X[i * D->nl + sel[p]]; rss += (pr - D->y[i]) * (pr - D->y[i]); }
    memcpy(coef, b, sizeof(double) * m); return rss;
}
/* description-length score in bits: residual code length (n * log2 of the rmse in micro units,
 * floor 1 micro) plus the spec 6.2 description bits of the terms */
static double term_bits(unsigned ni) { return 8.0 * ni + 24.0; }
static double resid_bits(double rss, uint32_t n) { double rmse_micro = n ? sqrt(rss / n) * 1e6 : 0; if (rmse_micro < 1) rmse_micro = 1; return n * log2(rmse_micro); }

typedef struct { unsigned sel[PD0L_MAX_SEL]; unsigned m; double coef[PD0L_MAX_SEL]; double rss, mdl; } eqfit;

/* forward selection with backward pruning for one target; fills best (MDL-optimal) and alt (the
 * runner-up subset: one term fewer if it exists, else one term more). deterministic order. */
static void select_terms(const design *D, unsigned ni, unsigned max_terms, eqfit *best, eqfit *alt)
{
    eqfit path[PD0L_MAX_SEL + 1]; memset(path, 0, sizeof path);
    path[0].m = 0; path[0].rss = fit_subset(D, path[0].sel, 0, path[0].coef); path[0].mdl = resid_bits(path[0].rss, D->n);
    unsigned steps = 0;
    for (unsigned round = 1; round <= max_terms; round++) {
        const eqfit *prev = &path[round - 1]; double best_rss = INFINITY; unsigned bk = D->nl; double bc[PD0L_MAX_SEL];
        for (unsigned k = 0; k < D->nl; k++) { int used = 0; for (unsigned s = 0; s < prev->m; s++) if (prev->sel[s] == k) used = 1; if (used) continue;
            unsigned cand[PD0L_MAX_SEL]; memcpy(cand, prev->sel, sizeof(unsigned) * prev->m); cand[prev->m] = k; double c[PD0L_MAX_SEL];
            double rss = fit_subset(D, cand, prev->m + 1, c);
            if (rss < best_rss * (1 - 1e-12)) { best_rss = rss; bk = k; memcpy(bc, c, sizeof c); } }
        if (bk == D->nl) break;
        eqfit *e = &path[round]; *e = *prev; e->sel[e->m++] = bk; memcpy(e->coef, bc, sizeof bc); e->rss = best_rss; e->mdl = resid_bits(best_rss, D->n) + term_bits(ni) * e->m; steps = round;
    }
    unsigned bi = 0; for (unsigned r = 1; r <= steps; r++) if (path[r].mdl < path[bi].mdl - 1e-9) bi = r;
    eqfit b = path[bi];
    /* backward pruning: drop any term whose removal does not worsen MDL */
    int changed = 1;
    while (changed && b.m > 0) { changed = 0;
        for (unsigned drop = 0; drop < b.m; drop++) { eqfit c = b; for (unsigned s = drop; s + 1 < c.m; s++) c.sel[s] = c.sel[s + 1]; c.m--;
            c.rss = fit_subset(D, c.sel, c.m, c.coef); c.mdl = resid_bits(c.rss, D->n) + term_bits(ni) * c.m;
            if (c.mdl <= b.mdl + 1e-9) { b = c; changed = 1; break; } } }
    *best = b;
    /* runner-up: one fewer (weakest by refit) if possible, else one more from the forward path */
    if (b.m > 0) { eqfit bestdrop; bestdrop.mdl = INFINITY; bestdrop.m = 0;
        for (unsigned drop = 0; drop < b.m; drop++) { eqfit c = b; for (unsigned s = drop; s + 1 < c.m; s++) c.sel[s] = c.sel[s + 1]; c.m--; c.rss = fit_subset(D, c.sel, c.m, c.coef); c.mdl = resid_bits(c.rss, D->n) + term_bits(ni) * c.m; if (c.mdl < bestdrop.mdl) bestdrop = c; }
        *alt = bestdrop; }
    else if (steps >= 1) *alt = path[1];
    else *alt = b;
}

static void quantise(const pd0_learner *L, const eqfit *f, uint8_t target, pd0_eq *q)
{
    memset(q, 0, sizeof *q); q->target = target; unsigned m = 0;
    for (unsigned s = 0; s < f->m; s++) { int64_t c = (int64_t)llround(f->coef[s] * 1e6); if (c == 0) continue; q->coef[m] = c; memcpy(q->expo[m], L->lib[f->sel[s]], L->ni); m++; }
    q->n_terms = (uint16_t)m;
}
static void finish_candidate(const pd0_learner *L, pd0l_candidate *c)
{
    c->rel.description_bits = pd0_rel_bits(&c->rel); c->bits = c->rel.description_bits; c->size = pd0_rel_size(&c->rel);
    c->fit_nrmse_micro = onestep_eval(L, &c->rel, TAG_FIT, c->lag1_autocorr);
    c->select_nrmse_micro = onestep_eval(L, &c->rel, TAG_SELECT, NULL);
    /* MDL on SELECT with the integer evaluator: residual bits per variable + description bits */
    uint8_t no = L->d.n_obs; double rb = 0; uint32_t n = 0; double se[PD0_MAX_OBS] = { 0 }; int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; memset(st, 0, sizeof st);
    for (uint32_t i = 0; i < L->nt; i++) { const trans *t = &L->t[i]; if (t->tag != TAG_SELECT) continue; if (t->is_reset) { memset(st, 0, sizeof st); continue; }
        for (int v = 0; v < no; v++) st[v] = t->before[v];
        pd0_rel_step(&c->rel, st, t->chan, t->value, nx);
        for (int v = 0; v < no; v++) { double e = (double)(t->after[v] - nx[v]) / 1e6; se[v] += e * e; } n++; memcpy(st, nx, sizeof st); }
    for (int v = 0; v < no; v++) rb += resid_bits(se[v], n);
    c->mdl_bits = rb + c->bits;
    c->hidden_state_suspected = 0; for (int v = 0; v < no; v++) if (fabs(c->lag1_autocorr[v]) > 0.3 && c->fit_nrmse_micro > 5000) c->hidden_state_suspected = 1;
}
static int cand_cmp(const void *a, const void *b) { const pd0l_candidate *x = a, *y = b; if (x->mdl_bits < y->mdl_bits) return -1; if (x->mdl_bits > y->mdl_bits) return 1; return (int)x->size - (int)y->size; }
static int same_structure(const pd0_rel *a, const pd0_rel *b) { return pd0_rel_size(a) == pd0_rel_size(b) && pd0_rel_bits(a) == pd0_rel_bits(b) && memcmp(a->eq, b->eq, sizeof a->eq) == 0; }

int pd0_learner_fit(pd0_learner *L)
{
    uint8_t no = L->d.n_obs; unsigned ni = L->ni, nl = L->nl; L->ncand = 0;
    uint32_t n = 0; for (uint32_t i = 0; i < L->nt; i++) if (L->t[i].tag == TAG_FIT && !L->t[i].is_reset) n++;
    pd0l_candidate *nullc = &L->cand[L->ncand++]; memset(nullc, 0, sizeof *nullc); nullc->is_null = 1;
    nullc->rel.n_vars = no; nullc->rel.n_channels = L->d.n_channels; nullc->rel.n_equations = 0; finish_candidate(L, nullc);
    if (n < 8) return L->ncand;
    double *X = malloc(sizeof(double) * n * nl), *Y = malloc(sizeof(double) * n * no); if (!X || !Y) { free(X); free(Y); return L->ncand; }
    uint32_t r = 0;
    for (uint32_t i = 0; i < L->nt; i++) { const trans *t = &L->t[i]; if (t->tag != TAG_FIT || t->is_reset) continue;
        double x[PD0_MAX_VARS + PD0_MAX_CHAN]; for (int j = 0; j < no; j++) x[j] = (double)t->before[j] / 1e6; for (int c = 0; c < L->d.n_channels; c++) x[no + c] = t->chan == c ? (double)t->value / 1e6 : 0;
        for (unsigned k = 0; k < nl; k++) X[r * nl + k] = mono(x, L->lib[k], ni);
        for (int j = 0; j < no; j++) Y[r * no + j] = (double)(t->after[j] - t->before[j]) / 1e6;
        r++; }
    double *y = malloc(sizeof(double) * n); eqfit best[PD0_MAX_OBS], alt[PD0_MAX_OBS];
    for (int tg = 0; tg < no; tg++) { for (uint32_t i = 0; i < n; i++) y[i] = Y[i * no + tg]; design D = { X, y, n, nl }; select_terms(&D, ni, PD0L_MAX_SEL, &best[tg], &alt[tg]); }
    /* candidate 1: MDL-best per target; alternatives: swap one target's equation for its runner-up */
    pd0l_candidate tmp[PD0L_MAX_CAND]; int nt = 0;
    for (int which = -1; which < (int)no && nt < PD0L_MAX_CAND - 1; which++) {
        pd0l_candidate *c = &tmp[nt]; memset(c, 0, sizeof *c); c->rel.n_vars = no; c->rel.n_channels = L->d.n_channels; unsigned ne = 0;
        for (int tg = 0; tg < no; tg++) { const eqfit *f = (tg == which) ? &alt[tg] : &best[tg]; pd0_eq q; quantise(L, f, (uint8_t)tg, &q); if (q.n_terms) c->rel.eq[ne++] = q; }
        c->rel.n_equations = (uint8_t)ne; finish_candidate(L, c);
        int dup = 0; for (int k = 0; k < nt; k++) if (same_structure(&tmp[k].rel, &c->rel)) dup = 1; if (!dup && (ne || which < 0)) nt++; }
    for (int k = 0; k < nt && L->ncand < PD0L_MAX_CAND; k++) if (!same_structure(&tmp[k].rel, &nullc->rel)) L->cand[L->ncand++] = tmp[k];
    qsort(L->cand, (size_t)L->ncand, sizeof L->cand[0], cand_cmp);
    free(X); free(Y); free(y);
    return L->ncand;
}
int pd0_learner_n_candidates(const pd0_learner *L) { return L->ncand; }
const pd0l_candidate *pd0_learner_candidate(const pd0_learner *L, int i) { return (i >= 0 && i < L->ncand) ? &L->cand[i] : NULL; }

/* ---- correlation evidence (spec T2) ---- */
static double pearson(const double *x, const double *y, uint32_t n)
{
    double mx = 0, my = 0; for (uint32_t i = 0; i < n; i++) { mx += x[i]; my += y[i]; } mx /= n; my /= n;
    double sxy = 0, sxx = 0, syy = 0; for (uint32_t i = 0; i < n; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) * (x[i] - mx); syy += (y[i] - my) * (y[i] - my); }
    return (sxx > 0 && syy > 0) ? sxy / sqrt(sxx * syy) : 0;
}
int pd0_learner_correlation(const pd0_learner *L, uint64_t shuffle_seed, uint32_t n_shuffles, pd0_corr *out)
{
    uint8_t no = L->d.n_obs; uint32_t n = 0; for (uint32_t i = 0; i < L->nt; i++) if (!L->t[i].is_reset) n++;
    if (n < 100) return -1;
    double *x = malloc(sizeof(double) * n), *y = malloc(sizeof(double) * n), *yp = malloc(sizeof(double) * n); if (!x || !y || !yp) { free(x); free(y); free(yp); return -1; }
    double best_r = 0; int ba = -1, bb = -1; uint32_t n_pairs = (uint32_t)(no + 1) * no;
    for (int a = -1; a < (int)no; a++) for (int b = 0; b < no; b++) { uint32_t k = 0;
        for (uint32_t i = 0; i < L->nt; i++) { const trans *t = &L->t[i]; if (t->is_reset) continue; x[k] = a < 0 ? (double)t->value : (double)t->before[a]; y[k] = (double)(t->after[b] - t->before[b]); k++; }
        double r = pearson(x, y, n); if (fabs(r) > fabs(best_r)) { best_r = r; ba = a; bb = b; } }
    if (bb < 0) { free(x); free(y); free(yp); return -1; }
    uint32_t k = 0; for (uint32_t i = 0; i < L->nt; i++) { const trans *t = &L->t[i]; if (t->is_reset) continue; x[k] = ba < 0 ? (double)t->value : (double)t->before[ba]; y[k] = (double)(t->after[bb] - t->before[bb]); k++; }
    pd0_rng g; pd0_rng_stream(&g, shuffle_seed, "perm"); uint32_t hits = 0; double r0 = fabs(best_r);
    for (uint32_t s = 0; s < n_shuffles; s++) { memcpy(yp, y, sizeof(double) * n); for (uint32_t i = n - 1; i > 0; i--) { uint32_t j = (uint32_t)(pd0_rng_next(&g) % (i + 1)); double t = yp[i]; yp[i] = yp[j]; yp[j] = t; } if (fabs(pearson(x, yp, n)) >= r0) hits++; }
    double p = (hits + 1.0) / (n_shuffles + 1.0) * n_pairs; if (p > 1) p = 1;
    memset(out, 0, sizeof *out); out->var_a = ba < 0 ? PD0_CHAN_NONE : (uint8_t)ba; out->var_b = (uint8_t)bb; out->n = n; out->r_micro = (int64_t)(best_r * 1e6); out->p_micro = (int64_t)(p * 1e6); out->n_pairs = n_pairs; out->n_shuffles = n_shuffles; out->shuffle_seed = shuffle_seed;
    free(x); free(y); free(yp); return 0;
}

/* ---- planner stand-in (spec section 8 selection rule, without local refinement) ---- */
int64_t pd0_learner_propose(const pd0_learner *L, const pd0_rel *hyps, int n_hyp, uint64_t planner_seed, uint32_t n_random,
                            const uint8_t (*forbidden)[PD0_HASH], uint32_t n_forbidden, pd0_exp *out)
{
    uint8_t no = L->d.n_obs; uint32_t n = PD0_MAX_STEPS; if (n_hyp < 2 || n_hyp > PD0_MAX_HYP) return -1;
    /* pooled residual sd of the hypotheses on SELECT (in units); floor 1e-4 */
    double r = 0; int cnt = 0; for (int h = 0; h < n_hyp; h++) { int64_t e = pd0_learner_onestep_nrmse(L, &hyps[h], TAG_SELECT); if (e > 0 && e < INT64_MAX) { r += (double)e / 1e6; cnt++; } }
    r = cnt ? r / cnt : 0; if (r < 1e-4) r = 1e-4;
    double bestD = -1; int64_t best_cost = INT64_MAX; pd0_exp best; memset(&best, 0, sizeof best); uint8_t best_hash[PD0_HASH] = { 0 };
    int64_t pred[PD0_MAX_HYP][PD0_MAX_STEPS * PD0_MAX_OBS];
    for (uint32_t idx = 0; idx < n_random; idx++) {
        pd0_exp e; memset(&e, 0, sizeof e); e.n_obs = no; e.n_hyp = (uint8_t)n_hyp; e.n_steps = (uint8_t)n;
        pd0_learner_random_schedule(&L->d, planner_seed, "plan", idx, e.reset, e.steps, n);
        uint8_t h[PD0_HASH]; pd0_schedule_hash(no, e.reset, (uint8_t)n, e.steps, h); int bad = 0;
        for (uint32_t f = 0; f < n_forbidden; f++) if (!memcmp(h, forbidden[f], PD0_HASH)) { bad = 1; break; }
        if (bad) continue;
        for (int hy = 0; hy < n_hyp; hy++) pd0_learner_rollout(&hyps[hy], e.reset, e.steps, n, pred[hy]);
        /* safety: the candidate's own prediction must stay well inside the world box */
        for (uint32_t s = 0; s < n && !bad; s++) for (int j = 0; j < no; j++) if (llabs(pred[0][s * no + j]) > 8000000) { bad = 1; break; }
        if (bad) continue;
        double D = 0; for (int a = 0; a < n_hyp; a++) for (int b = a + 1; b < n_hyp; b++) for (uint32_t s = 0; s < n; s++) for (int j = 0; j < no; j++) { double d = fabs((double)(pred[a][s * no + j] - pred[b][s * no + j])) / 1e6 / r; if (d > D) D = d; }
        int64_t cost = 0; for (uint32_t s = 0; s < n; s++) cost += llabs(e.steps[s].value);
        int better = D > bestD + 1e-12 || (fabs(D - bestD) <= 1e-12 && (cost < best_cost || (cost == best_cost && memcmp(h, best_hash, PD0_HASH) < 0)));
        if (better) { bestD = D; best_cost = cost; best = e; memcpy(best_hash, h, PD0_HASH); for (int hy = 0; hy < n_hyp; hy++) for (uint32_t s = 0; s < n; s++) for (int j = 0; j < no; j++) best.expected[hy][s][j] = pred[hy][s * no + j]; }
    }
    if (bestD < 3.0) return -1;   /* NO_DISCRIMINATING_EXPERIMENT */
    best.divergence_micro = (int64_t)(bestD * 1e6); memcpy(best.schedule_hash, best_hash, PD0_HASH); *out = best; return best.divergence_micro;
}

size_t pd0_learner_report(const pd0_learner *L, char *buf, size_t cap)
{
    size_t n = 0; n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "{\"n_fit\":%u,\"n_select\":%u,\"candidates\":[", pd0_learner_n_transitions(L, TAG_FIT), pd0_learner_n_transitions(L, TAG_SELECT));
    for (int i = 0; i < L->ncand; i++) { const pd0l_candidate *c = &L->cand[i];
        n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%s{\"rank\":%d,\"null\":%d,\"size\":%u,\"bits\":%u,\"fit_nrmse_micro\":%lld,\"select_nrmse_micro\":%lld,\"mdl_bits\":%.1f,\"hidden_state_suspected\":%d,\"eq\":[",
                              i ? "," : "", i, c->is_null, c->size, c->bits, (long long)c->fit_nrmse_micro, (long long)c->select_nrmse_micro, c->mdl_bits, c->hidden_state_suspected);
        for (int e = 0; e < c->rel.n_equations; e++) { const pd0_eq *q = &c->rel.eq[e]; n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%s{\"target\":%u,\"terms\":[", e ? "," : "", q->target);
            for (int t = 0; t < q->n_terms; t++) { n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%s{\"coef\":%lld,\"expo\":[", t ? "," : "", (long long)q->coef[t]);
                for (unsigned k = 0; k < (unsigned)(c->rel.n_vars + c->rel.n_channels); k++) n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%s%u", k ? "," : "", q->expo[t][k]);
                n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "]}"); }
            n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "]}"); }
        n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "]}"); }
    n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "]}");
    return n < cap ? n : cap - 1;
}
