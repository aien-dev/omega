#include "pd0_score.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int pd0_rel_rollout_fn(void *ctx, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out)
{
    const pd0_rel *r = ctx; int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; uint8_t no = (uint8_t)(r->n_vars - r->n_latent);
    memset(st, 0, sizeof st); for (int j = 0; j < no; j++) st[j] = init[j];
    for (uint32_t s = 0; s < n; s++) { pd0_rel_step(r, st, steps[s].channel, steps[s].value, nx); for (int j = 0; j < no; j++) out[s * no + j] = nx[j]; memcpy(st, nx, sizeof st); }
    return 0;
}
int pd0_rel_predict_fn(void *ctx, const int64_t *state, uint8_t chan, int64_t value, int64_t *next)
{
    const pd0_rel *r = ctx; int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; uint8_t no = (uint8_t)(r->n_vars - r->n_latent);
    memset(st, 0, sizeof st); for (int j = 0; j < no; j++) st[j] = state[j];
    pd0_rel_step(r, st, chan, value, nx); for (int j = 0; j < no; j++) next[j] = nx[j]; return 0;
}

typedef struct { double se[PD0_MAX_OBS], s[PD0_MAX_OBS], ss[PD0_MAX_OBS]; uint32_t n; } acc;
static void acc_add(acc *a, const int64_t *p, const int64_t *t, uint8_t no) { for (int j = 0; j < no; j++) { double e = (double)(p[j] - t[j]) / 1e6, v = (double)t[j] / 1e6; a->se[j] += e * e; a->s[j] += v; a->ss[j] += v * v; } a->n++; }
static int64_t acc_nrmse(const acc *a, uint8_t no)
{
    if (!a->n) return 0; double w = 0;
    for (int j = 0; j < no; j++) { double rmse = sqrt(a->se[j] / a->n), m = a->s[j] / a->n, var = a->ss[j] / a->n - m * m, sd = var > 0 ? sqrt(var) : 0, v = sd > 1e-9 ? rmse / sd : rmse; if (v > w) w = v; }
    return w > 9e12 ? INT64_MAX : (int64_t)(w * 1e6);
}
static int term_matches(const pd0_rel *r, const pd0_true_term *t, int64_t tol_ppm, int check_coef, int64_t *found_coef)
{
    unsigned ne = r->n_vars + r->n_channels;
    for (int e = 0; e < r->n_equations; e++) { if (r->eq[e].target != t->target) continue;
        for (int k = 0; k < r->eq[e].n_terms; k++) { if (r->eq[e].coef[k] == 0) continue; if (memcmp(r->eq[e].expo[k], t->expo, ne)) continue;
            *found_coef = r->eq[e].coef[k]; if (!check_coef) return 1;
            int64_t d = llabs(r->eq[e].coef[k] - t->coef), lim = (llabs(t->coef) * tol_ppm) / 1000000; return d <= lim ? 1 : 2; } }
    return 0;
}

static void set_fail(pd0_score_result *o, int code) { o->fail_mask |= PD0_FAIL_BIT(code); if (!o->code) o->code = code; }

int pd0_score(const pd0_score_params *P, const pd0_rel *rel, uint32_t stored, pd0_rollout_fn roll, pd0_predict_fn pred, void *ctx,
              const pd0_score_episode *eps, uint32_t n_eps, const pd0_transition *fit, uint32_t n_fit, pd0_score_result *out)
{
    memset(out, 0, sizeof *out); uint8_t no = P->n_obs;
    if (rel->n_vars != no + rel->n_latent || rel->n_channels != P->n_channels) { out->code = PD0V_BAD_FIELD; return out->code; }
    /* structure first (M5: these must bite regardless of bounds) */
    if (pd0_rel_max_degree(rel) > 3) set_fail(out, PD0V_SCORE_DEGREE);
    out->size = pd0_rel_size(rel) + stored; out->bits = pd0_rel_bits(rel) + 24u * stored;
    if (out->size > P->size_bound) set_fail(out, PD0V_SCORE_SIZE);
    if (rel->description_bits != pd0_rel_bits(rel)) set_fail(out, PD0V_SCORE_BITS_MISMATCH);
    for (uint32_t t = 0; t < P->n_true_terms; t++) { int64_t c; int m = term_matches(rel, &P->true_terms[t], P->const_tol_ppm, P->score_constants, &c);
        if (m == 0) set_fail(out, PD0V_SCORE_SUPPORT_MISSING); else if (m == 2) set_fail(out, PD0V_SCORE_CONSTANT_OFF); }
    if (P->require_latent && rel->n_latent == 0) set_fail(out, PD0V_SCORE_L6_LATENT_MISSING);
    /* 6.1 rollouts */
    acc ain, aex, a1; memset(&ain, 0, sizeof ain); memset(&aex, 0, sizeof aex); memset(&a1, 0, sizeof a1);
    int64_t pr[PD0_MAX_STEPS * PD0_MAX_OBS], nx[PD0_MAX_OBS];
    for (uint32_t i = 0; i < n_eps; i++) { const pd0_score_episode *e = &eps[i];
        if (roll(ctx, e->init, e->steps, PD0_MAX_STEPS, pr)) return (out->code = PD0V_BAD_FIELD);
        for (uint32_t s = 0; s < PD0_MAX_STEPS; s++) acc_add(e->in_box ? &ain : &aex, pr + s * no, e->truth[s], no);
        if (P->onestep_bound && pred) { const int64_t *st = e->init; for (uint32_t s = 0; s < PD0_MAX_STEPS; s++) { if (pred(ctx, st, e->steps[s].channel, e->steps[s].value, nx)) return (out->code = PD0V_BAD_FIELD); acc_add(&a1, nx, e->truth[s], no); st = e->truth[s]; } } }
    out->inbox_nrmse = acc_nrmse(&ain, no); out->extrap_nrmse = acc_nrmse(&aex, no); out->onestep_nrmse = acc_nrmse(&a1, no);
    if (out->inbox_nrmse > P->inbox_bound) set_fail(out, PD0V_SCORE_INBOX_NRMSE);
    if (aex.n && out->extrap_nrmse > P->extrap_bound) set_fail(out, PD0V_SCORE_EXTRAP_NRMSE);
    if (P->onestep_bound && a1.n && out->onestep_nrmse > P->onestep_bound) set_fail(out, PD0V_SCORE_ONESTEP_NRMSE);
    /* L6: harness-fitted latent-free reference must fail by the factor */
    if (P->require_latent && n_fit) { pd0_rel ref; if (pd0_reference_fit(no, P->n_channels, P->size_bound, fit, n_fit, &ref) == 0) { acc ar; memset(&ar, 0, sizeof ar);
            for (uint32_t i = 0; i < n_eps; i++) if (eps[i].in_box) { pd0_rel_rollout_fn(&ref, eps[i].init, eps[i].steps, PD0_MAX_STEPS, pr); for (uint32_t s = 0; s < PD0_MAX_STEPS; s++) acc_add(&ar, pr + s * no, eps[i].truth[s], no); }
            out->ref_nrmse = acc_nrmse(&ar, no); if (out->ref_nrmse < P->latent_ref_factor * P->inbox_bound) set_fail(out, PD0V_SCORE_L6_REFERENCE_TOO_GOOD); } }
    return out->code;
}

/* ---- reference sparse least squares (harness side) ---- */
#define LIB_MAX 128
static unsigned build_library(unsigned ni, uint8_t lib[LIB_MAX][PD0_MAX_VARS + PD0_MAX_CHAN])
{
    unsigned n = 0; uint8_t e[PD0_MAX_VARS + PD0_MAX_CHAN];
    /* all exponent vectors with total degree <= 3 over ni inputs */
    for (unsigned a = 0; a <= ni; a++) for (unsigned b = a; b <= ni; b++) for (unsigned c = b; c <= ni; c++) {
        memset(e, 0, sizeof e); if (a < ni) e[a]++; if (b < ni) e[b]++; if (c < ni) e[c]++;
        int dup = 0; for (unsigned k = 0; k < n; k++) if (!memcmp(lib[k], e, ni)) { dup = 1; break; }
        if (!dup && n < LIB_MAX) memcpy(lib[n++], e, sizeof e); }
    return n;
}
static double mono(const double *x, const uint8_t *e, unsigned ni) { double v = 1; for (unsigned i = 0; i < ni; i++) for (uint8_t k = 0; k < e[i]; k++) v *= x[i]; return v; }
static int solve(double *A, double *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) { unsigned p = i; for (unsigned r = i + 1; r < n; r++) if (fabs(A[r * n + i]) > fabs(A[p * n + i])) p = r;
        if (fabs(A[p * n + i]) < 1e-12) return -1;
        if (p != i) { for (unsigned k = 0; k < n; k++) { double t = A[i * n + k]; A[i * n + k] = A[p * n + k]; A[p * n + k] = t; } double t = b[i]; b[i] = b[p]; b[p] = t; }
        for (unsigned r = i + 1; r < n; r++) { double f = A[r * n + i] / A[i * n + i]; for (unsigned k = i; k < n; k++) A[r * n + k] -= f * A[i * n + k]; b[r] -= f * b[i]; } }
    for (int i = (int)n - 1; i >= 0; i--) { double s = b[i]; for (unsigned k = (unsigned)i + 1; k < n; k++) s -= A[i * n + k] * b[k]; b[i] = s / A[i * n + i]; }
    return 0;
}
int pd0_reference_fit(uint8_t no, uint8_t nc, uint32_t max_terms, const pd0_transition *fit, uint32_t n_fit, pd0_rel *out)
{
    unsigned ni = no + nc; uint8_t lib[LIB_MAX][PD0_MAX_VARS + PD0_MAX_CHAN]; unsigned nl = build_library(ni, lib);
    if (n_fit < 8 || n_fit > PD0_MAX_FIT || max_terms == 0) return -1; if (max_terms > 16) max_terms = 16;
    double *X = malloc(sizeof(double) * n_fit * nl), *y = malloc(sizeof(double) * n_fit); if (!X || !y) { free(X); free(y); return -1; }
    for (uint32_t i = 0; i < n_fit; i++) { double x[PD0_MAX_VARS + PD0_MAX_CHAN]; for (int j = 0; j < no; j++) x[j] = (double)fit[i].before[j] / 1e6; for (int c = 0; c < nc; c++) x[no + c] = fit[i].chan == c ? (double)fit[i].value / 1e6 : 0;
        for (unsigned k = 0; k < nl; k++) X[i * nl + k] = mono(x, lib[k], ni); }
    memset(out, 0, sizeof *out); out->n_vars = no; out->n_latent = 0; out->n_channels = nc; out->n_equations = no;
    for (int t = 0; t < no; t++) {
        for (uint32_t i = 0; i < n_fit; i++) y[i] = (double)(fit[i].after[t] - fit[i].before[t]) / 1e6;
        unsigned sel[16], ns = 0; double coef[16] = { 0 };
        for (uint32_t round = 0; round < max_terms; round++) { double best_rss = INFINITY; unsigned best = nl; double bestc[16];
            for (unsigned k = 0; k < nl; k++) { int used = 0; for (unsigned s = 0; s < ns; s++) if (sel[s] == k) used = 1; if (used) continue;
                unsigned cand[16]; memcpy(cand, sel, sizeof(unsigned) * ns); cand[ns] = k; unsigned m = ns + 1;
                double A[256] = { 0 }, b[16] = { 0 };
                for (uint32_t i = 0; i < n_fit; i++) for (unsigned p = 0; p < m; p++) { double xp = X[i * nl + cand[p]]; b[p] += xp * y[i]; for (unsigned q = 0; q < m; q++) A[p * m + q] += xp * X[i * nl + cand[q]]; }
                for (unsigned p = 0; p < m; p++) A[p * m + p] += 1e-9;
                if (solve(A, b, m)) continue;
                double rss = 0; for (uint32_t i = 0; i < n_fit; i++) { double pr = 0; for (unsigned p = 0; p < m; p++) pr += b[p] * X[i * nl + cand[p]]; rss += (pr - y[i]) * (pr - y[i]); }
                if (rss < best_rss) { best_rss = rss; best = k; memcpy(bestc, b, sizeof(double) * m); } }
            if (best == nl) break; sel[ns] = best; ns++; memcpy(coef, bestc, sizeof(double) * ns); }
        pd0_eq *q = &out->eq[t]; q->target = (uint8_t)t; q->n_terms = (uint16_t)ns;
        for (unsigned s = 0; s < ns; s++) { q->coef[s] = (int64_t)llround(coef[s] * 1e6); memcpy(q->expo[s], lib[sel[s]], ni); } }
    out->description_bits = pd0_rel_bits(out);
    free(X); free(y); return 0;
}
