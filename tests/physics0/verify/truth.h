/* TEST-ONLY truth generators, copied from the spec equations (section 4.1).
 * Allowed in tests; never in a verifier or learner code path. */
#ifndef PD0_TEST_TRUTH_H
#define PD0_TEST_TRUTH_H
#include "pd0_fmt.h"
#include "pd0_rng.h"
#include "pd0_score.h"
#include <string.h>
typedef struct { int level; int64_t dt, k, c, b, m, w, q; uint8_t n_obs, n_hidden; } truth_world;
static int64_t draw_const(pd0_rng *g, int64_t lo, int64_t hi) { return lo + ((hi - lo) * pd0_rng_unit(g)) / PD0_MICRO; }
static void truth_init(truth_world *w, int level, uint64_t seed)
{
    pd0_rng g; pd0_rng_stream(&g, seed, "const"); memset(w, 0, sizeof *w); w->level = level; w->dt = 50000; w->n_obs = 2;
    switch (level) {
    case 1: w->k = draw_const(&g, 1000000, 4000000); break;
    case 2: w->k = draw_const(&g, 1000000, 9000000); w->c = draw_const(&g, 200000, 2000000); break;
    case 4: w->k = draw_const(&g, 1000000, 4000000); w->b = draw_const(&g, 500000, 2000000); break;
    case 6: w->k = draw_const(&g, 2000000, 5000000); w->m = draw_const(&g, 500000, 1000000); w->w = draw_const(&g, 1000000, 2000000); w->q = draw_const(&g, 500000, 1000000); w->n_hidden = 1; break;
    default: w->k = 2000000; w->level = 1;
    }
}
/* state = observed then hidden */
static void truth_step(const truth_world *w, const int64_t *s, int64_t u, int64_t *n)
{
    int64_t dt = w->dt; n[0] = s[0] + pd0_mul(s[1], dt);
    switch (w->level) {
    case 1: n[1] = s[1] + pd0_mul(-pd0_mul(w->k, s[0]) + u, dt); break;
    case 2: n[1] = s[1] + pd0_mul(-pd0_mul(w->k, s[0]) - pd0_mul(w->c, s[1]) + u, dt); break;
    case 4: n[1] = s[1] + pd0_mul(-pd0_mul(w->k, s[0]) - pd0_mul(w->b, pd0_mul(s[0], pd0_mul(s[0], s[0]))) + u, dt); break;
    case 6: n[1] = s[1] + pd0_mul(-pd0_mul(w->k, s[0]) + pd0_mul(w->m, s[2]) + u, dt); n[2] = s[2] + pd0_mul(-pd0_mul(w->w, s[2]) + pd0_mul(w->q, s[0]), dt); break;
    default: n[1] = s[1];
    }
}
/* the exact discrete-map relation (what an oracle handed the form would recover) */
static void add_term(pd0_eq *q, int64_t coef, int i0, int e0, int i1, int e1)
{
    int t = q->n_terms++; q->coef[t] = coef; memset(q->expo[t], 0, sizeof q->expo[t]); if (i0 >= 0) q->expo[t][i0] = (uint8_t)e0; if (i1 >= 0) q->expo[t][i1] = (uint8_t)e1;
}
static void truth_oracle_rel(const truth_world *w, pd0_rel *r)
{
    memset(r, 0, sizeof *r); r->n_vars = (uint8_t)(w->n_obs + w->n_hidden); r->n_latent = w->n_hidden; r->n_channels = 1; r->n_equations = r->n_vars;
    int U = r->n_vars; int64_t dt = w->dt;
    r->eq[0].target = 0; add_term(&r->eq[0], dt, 1, 1, -1, 0);
    r->eq[1].target = 1; add_term(&r->eq[1], -pd0_mul(w->k, dt), 0, 1, -1, 0); add_term(&r->eq[1], dt, U, 1, -1, 0);
    if (w->level == 2) add_term(&r->eq[1], -pd0_mul(w->c, dt), 1, 1, -1, 0);
    if (w->level == 4) add_term(&r->eq[1], -pd0_mul(w->b, dt), 0, 3, -1, 0);
    if (w->level == 6) { add_term(&r->eq[1], pd0_mul(w->m, dt), 2, 1, -1, 0); r->eq[2].target = 2; add_term(&r->eq[2], -pd0_mul(w->w, dt), 2, 1, -1, 0); add_term(&r->eq[2], pd0_mul(w->q, dt), 0, 1, -1, 0); }
    r->description_bits = pd0_rel_bits(r);
}
static void truth_score_params(const truth_world *w, pd0_score_params *P)
{
    memset(P, 0, sizeof *P); P->n_obs = 2; P->n_channels = 1; P->const_tol_ppm = 50000; P->score_constants = 1;
    P->inbox_bound = 20000; P->extrap_bound = 60000; P->onestep_bound = 10000; P->latent_ref_factor = 3;
    pd0_rel r; truth_oracle_rel(w, &r); P->size_bound = pd0_rel_size(&r) + 2;
    if (w->level == 6) { P->score_constants = 0; P->require_latent = 1; P->inbox_bound = 30000; P->extrap_bound = 80000; P->onestep_bound = 0; return; }
    for (int e = 0; e < r.n_equations; e++) for (int t = 0; t < r.eq[e].n_terms; t++) { pd0_true_term *tt = &P->true_terms[P->n_true_terms++]; tt->target = r.eq[e].target; memcpy(tt->expo, r.eq[e].expo[t], sizeof tt->expo); tt->coef = r.eq[e].coef[t]; }
}
/* scoring episodes: n_in inside [-2,2], n_ex in [-3,3]; schedules from the "score" stream */
static void truth_score_episodes(const truth_world *w, uint64_t seed, uint32_t n_in, uint32_t n_ex, pd0_score_episode *eps)
{
    pd0_rng g; pd0_rng_stream(&g, seed, "score");
    for (uint32_t i = 0; i < n_in + n_ex; i++) { pd0_score_episode *e = &eps[i]; memset(e, 0, sizeof *e); e->in_box = i < n_in; int64_t box = e->in_box ? 2000000 : 3000000;
        int64_t st[PD0_MAX_VARS] = { 0 }, nx[PD0_MAX_VARS] = { 0 };
        for (int j = 0; j < 2; j++) { e->init[j] = draw_const(&g, -box, box); st[j] = e->init[j]; }
        for (uint32_t s = 0; s < PD0_MAX_STEPS; s++) { e->steps[s].channel = 0; e->steps[s].value = draw_const(&g, -2000000, 2000000); truth_step(w, st, e->steps[s].value, nx); memcpy(st, nx, sizeof st); e->truth[s][0] = st[0]; e->truth[s][1] = st[1]; } }
}
static uint32_t truth_fit_transitions(const truth_world *w, uint64_t seed, uint32_t n_ep, uint32_t steps, pd0_transition *out, uint32_t cap)
{
    pd0_rng g; pd0_rng_stream(&g, seed, "fit"); uint32_t n = 0;
    for (uint32_t e = 0; e < n_ep; e++) { int64_t st[PD0_MAX_VARS] = { 0 }, nx[PD0_MAX_VARS] = { 0 }; st[0] = draw_const(&g, -2000000, 2000000); st[1] = draw_const(&g, -2000000, 2000000);
        for (uint32_t s = 0; s < steps && n < cap; s++) { pd0_transition *t = &out[n++]; t->before[0] = st[0]; t->before[1] = st[1]; t->chan = 0; t->value = draw_const(&g, -2000000, 2000000); truth_step(w, st, t->value, nx); memcpy(st, nx, sizeof st); t->after[0] = st[0]; t->after[1] = st[1]; } }
    return n;
}
#endif
