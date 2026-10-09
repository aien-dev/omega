/* BRW-ACT-DEV0 candidate: exact Bayesian updater over a fixed hypothesis grid and a greedy
 * information-per-time choice of the next wait time. Spec: docs/turing/BRW_ACT_DEV0_PROFILE.md
 * section 2. Three explanations (M0 diffusion, M1 drift-diffusion, M2 mean reversion), 528 grid
 * hypotheses, Gaussian reading per hypothesis per wait time, instrument sd 1. The library sees
 * only menu indices, costs and readings; the harness lives in tests/brownian/brw_active_dev0.c.
 * Mixture predictions leave through PRD2 family 2 (K <= 8, moment-matched tail). */
#ifndef BRW_ACTIVE_H
#define BRW_ACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "turing/ty_prd2.h"

#define BRW_NM 3                 /* explanations */
#define BRW_NH 528               /* 16 + 16*16 + 16*16 */
#define BRW_NT 6                 /* menu entries */
#define BRW_NQ 512               /* trapezoid points */
#define BRW_P_SURE 0.99          /* phase switch and verdict threshold */
#define BRW_W_ACTIVE 1e-12       /* posterior weight below which a hypothesis leaves the design */

extern const int brw_tau[BRW_NT];                 /* {1, 2, 4, 8, 16, 32} */
#define BRW_COST(ti) (brw_tau[ti] + 2)            /* reset + readout */

enum { BRW_V_INADEQUATE = 0, BRW_V_M0 = 1, BRW_V_M1 = 2, BRW_V_M2 = 3, BRW_V_UNDETERMINED = 4 };
const char *brw_verdict_name(int v);

typedef struct { int model; double D, v, theta; } brw_hyp;

typedef struct {
    brw_hyp h[BRW_NH];
    double mu[BRW_NT][BRW_NH], var[BRW_NT][BRW_NH];   /* predictive reading per hypothesis */
    double lnorm[BRW_NT][BRW_NH];                     /* -0.5 ln(2 pi var) */
    double lprior_m[BRW_NM];                          /* ln prior of each hypothesis, by model */
} brw_grid;
void brw_grid_init(brw_grid *g);

typedef struct {
    const brw_grid *g;
    double logw[BRW_NH];                              /* normalised log posterior */
    unsigned n_obs, n_pred, n_out99;                  /* readings, predictions checked, outside central 99% */
    uint64_t lik_evals, quad_evals, cdf_evals;        /* compute counters */
} brw_state;
void brw_state_init(brw_state *s, const brw_grid *g);

double brw_lse(const double *a, size_t n);            /* log-sum-exp, -inf safe */
void brw_model_post(const brw_state *s, double p[BRW_NM]);
double brw_cdf(brw_state *s, int ti, double y);       /* full posterior predictive CDF */
/* Prequential check on the prediction made before y, then exact update. Returns 1 if y fell
 * outside the central 99% interval, 0 if inside, -1 (state untouched) if y is not finite. */
int brw_observe(brw_state *s, int ti, double y);

unsigned brw_binom_crit(unsigned n);                  /* smallest c with P(Bin(n, 0.01) >= c) <= 0.001 */
int brw_verdict(const brw_state *s);

/* Quadrature layer on plain arrays (weights > 0 summing to 1). Grid [lo, hi] is the min/max of
 * mu -/+ 8 s over the components, BRW_NQ points, trapezoid rule. quad counts component density
 * evaluations. */
double brw_mix_entropy_bits(const double *w, const double *mu, const double *var, size_t n, uint64_t *quad);
/* I(Y;G) and I(Y;H) in bits for components tagged with group grp[i] in [0, ngrp), ngrp <= 8. */
void brw_info_arrays(size_t n, const double *w, const int *grp, const double *mu, const double *var,
                     int ngrp, double *i_group, double *i_hyp, uint64_t *quad);
/* I(Y;M) and I(Y;H) for wait time index ti from the current posterior (active hypotheses only,
 * renormalised). */
void brw_info(brw_state *s, int ti, double *i_model, double *i_hyp);

/* Index of the largest score among ok[] entries, ties to the smaller index; -1 if none ok. */
int brw_pick(const double *score, const int *ok, int n);
/* Greedy choice under `remaining` time units. Returns menu index or -1 if nothing is affordable.
 * *phase is set to 1 (model information) or 2 (hypothesis information) when non-NULL. */
int brw_choose(brw_state *s, int remaining, int *phase);

/* Reduce a mixture to K <= 8 PRD2 family-2 components: the 7 heaviest keep (w, mu, sd), the rest
 * merge by moment matching. Mixtures of <= 8 components are copied exactly. Weights are
 * renormalised; the result is checked by ty_prd2_validate (its status is returned). */
int brw_reduce(const double *w, const double *mu, const double *sd, size_t n, uint32_t index, tyq_pred *p);
/* The posterior predictive at menu index ti as a validated PRD2 record. */
int brw_predict_prd2(const brw_state *s, int ti, uint32_t index, tyq_pred *p);

#endif
