/* TEST-ONLY honest evidence-bundle builder with switchable dishonesty mutants. */
#ifndef PD0_TEST_BUNDLE_H
#define PD0_TEST_BUNDLE_H
#include "truth.h"
#include "pd0_ladder.h"
#include <stdlib.h>
#define LCAP (8u << 20)
#include "pd0_controls.h"
static uint32_t pd0_null_world_records(uint64_t seed, pd0_rec *out, uint32_t cap) { return pd0_null_world(seed, 2, 10, 100, -2000000, 2000000, -2000000, 2000000, out, cap); }
typedef struct {
    int skip_corr, fake_corr, no_rival, reuse_schedule, trial_without_prereg, wrong_k, rep_before_candidate, batch_seed_reuse, all_planner, eps_above_bound, stop_after_prereg, stop_after_trials, demote_after_law;
    int null_world; uint64_t null_seed;
} bundle_opts;
typedef struct {
    uint8_t *buf; size_t len; uint8_t last[PD0_HASH];
    uint8_t rec_prev[PD0_HASH]; uint64_t seq; uint32_t episode;
    truth_world w; pd0_ladder_params P; pd0_rng g;
    pd0_rec *recs; uint32_t nrec;
} bundle;
static inline void b_append(bundle *b, uint8_t kind, const uint8_t *p, uint32_t n) { b->len = pd0_ledg_append(b->buf, b->len, LCAP, kind, p, n, b->last); }
static inline void b_record(bundle *b, pd0_rec *r)
{
    uint8_t tmp[512]; memcpy(r->prev_hash, b->rec_prev, PD0_HASH); r->seq = b->seq++; r->n_obs = b->P.n_obs;
    size_t n = pd0_rec_write(r, tmp, sizeof tmp); memcpy(b->rec_prev, r->record_hash, PD0_HASH); b_append(b, LEDG_OBS, tmp, (uint32_t)n);
    if (b->recs && b->nrec < 65536) b->recs[b->nrec++] = *r;
}
static inline void b_tag(bundle *b, uint32_t ep, uint8_t tag, uint32_t batch, uint8_t origin) { pd0_tag t = { ep, tag, batch, origin }; uint8_t p[16]; b_append(b, LEDG_TAG, p, (uint32_t)pd0_tag_write(&t, p)); }
/* run one episode of the truth world (or the null world) with the given reset and schedule; returns the episode number */
static inline uint32_t b_episode(bundle *b, const int64_t *reset, const pd0_step *steps, uint32_t n, const truth_world *w)
{
    uint32_t ep = b->episode++; int64_t st[PD0_MAX_VARS] = { 0 }, nx[PD0_MAX_VARS] = { 0 }; pd0_rec r; memset(&r, 0, sizeof r);
    r.kind = PD0_KIND_RESET; r.channel = PD0_CHAN_NONE; r.episode = ep; for (int j = 0; j < 2; j++) { st[j] = reset[j]; r.before[j] = r.after[j] = reset[j]; } b_record(b, &r);
    for (uint32_t s = 0; s < n; s++) { memset(&r, 0, sizeof r); r.kind = PD0_KIND_STEP; r.episode = ep; r.step_in_episode = s + 1; r.time_micro = (int64_t)(s + 1) * b->P.dt_micro; r.channel = steps[s].channel; r.requested = r.applied = steps[s].value;
        r.before[0] = st[0]; r.before[1] = st[1]; truth_step(w, st, steps[s].value, nx); memcpy(st, nx, sizeof st); r.after[0] = st[0]; r.after[1] = st[1]; b_record(b, &r); }
    return ep;
}
static inline void b_random_schedule(bundle *b, int64_t *reset, pd0_step *steps, uint32_t n) { for (int j = 0; j < 2; j++) reset[j] = draw_const(&b->g, -2000000, 2000000); for (uint32_t s = 0; s < n; s++) { steps[s].channel = 0; steps[s].value = draw_const(&b->g, -2000000, 2000000); } }
static inline void b_init(bundle *b, int level, uint64_t seed)
{
    memset(b, 0, sizeof *b); b->buf = malloc(LCAP); b->recs = malloc(sizeof(pd0_rec) * 65536); truth_init(&b->w, level, seed); pd0_rng_stream(&b->g, seed, "bundle");
    pd0_ladder_params_default(&b->P, 2, 1); b->P.size_bound = 6; b->P.eps_bound_micro = 20000;
}
static inline void b_free(bundle *b) { free(b->buf); free(b->recs); }
/* Build the whole honest ladder (or a mutant). Returns the law bytes length via *law_len after running the checker. */
static inline int b_build(bundle *b, const bundle_opts *o, pd0_ladder_report *R)
{
    int64_t reset[2]; pd0_step steps[100]; const truth_world *w = &b->w; truth_world null_w = b->w;
    /* 1. collect 10 episodes of 100 steps */
    for (int e = 0; e < 10; e++) { b_random_schedule(b, reset, steps, 100); b_episode(b, reset, steps, 100, w); }
    if (o->null_world) { /* overwrite with null-world records: rebuild the ledger from scratch */
        pd0_rec *nr = malloc(sizeof(pd0_rec) * 2048); uint32_t n = pd0_null_world_records(o->null_seed, nr, 2048);
        b->len = 0; memset(b->last, 0, PD0_HASH); memset(b->rec_prev, 0, PD0_HASH); b->seq = 0; b->nrec = 0; b->episode = 10;
        for (uint32_t i = 0; i < n; i++) { pd0_rec r = nr[i]; b_record(b, &r); } free(nr); }
    /* 2. harness tags */
    for (uint32_t e = 0; e < 10; e++) b_tag(b, e, e < 6 ? TAG_FIT : e < 8 ? TAG_SELECT : TAG_HOLDOUT, 0, 0);
    if (o->rep_before_candidate) { b_tag(b, b->episode, TAG_REP, 99, 0); b_random_schedule(b, reset, steps, 20); b_episode(b, reset, steps, 20, w); }
    /* 3. correlation evidence: pair (s1 at t, change of s0) */
    if (!o->skip_corr) { double *x = malloc(sizeof(double) * 65536), *y = malloc(sizeof(double) * 65536); uint32_t n = 0;
        for (uint32_t i = 0; i < b->nrec; i++) { const pd0_rec *r = &b->recs[i]; if (r->kind != PD0_KIND_STEP || r->status != PD0_ST_OK || r->episode >= 8) continue; x[n] = (double)r->before[1]; y[n] = (double)(r->after[0] - r->before[0]); n++; }
        pd0_corr c; memset(&c, 0, sizeof c); c.var_a = 1; c.var_b = 0; c.n = n; c.n_pairs = 6; c.n_shuffles = 2000; c.shuffle_seed = 77;
        double r = pd0_pearson(x, y, n), p = pd0_perm_p(x, y, n, 2000, 77) * 6; c.r_micro = (int64_t)(r * 1e6); c.p_micro = (int64_t)(p * 1e6);
        if (o->fake_corr) { c.r_micro = 900000; c.p_micro = 100; }
        uint8_t pl[64]; b_append(b, LEDG_CORR, pl, (uint32_t)pd0_corr_write(&c, pl)); free(x); free(y); }
    /* 4. candidate + rival */
    pd0_rel cand, nullr; truth_oracle_rel(w, &cand); if (o->wrong_k) { uint8_t ex[16] = { 1, 0, 0 }; pd0_mut_scale_coef(&cand, 1, ex, 110, 100); }
    uint8_t pl[8192]; pl[0] = REL_CANDIDATE; size_t n = pd0_rel_write(&cand, pl + 1, sizeof pl - 1); b_append(b, LEDG_RELATION, pl, (uint32_t)n + 1); uint8_t cand_hash[PD0_HASH]; memcpy(cand_hash, b->last, PD0_HASH);
    memset(&nullr, 0, sizeof nullr); nullr.n_vars = 2; nullr.n_channels = 1; nullr.n_equations = 0; nullr.description_bits = 0;
    pl[0] = REL_NULL; n = pd0_rel_write(&nullr, pl + 1, sizeof pl - 1); b_append(b, LEDG_RELATION, pl, (uint32_t)n + 1); uint8_t null_hash[PD0_HASH]; memcpy(null_hash, b->last, PD0_HASH);
    /* 5. falsifier */
    pd0_falsifier f; memset(&f, 0, sizeof f); memcpy(f.candidate, cand_hash, PD0_HASH); f.n_rivals = o->no_rival ? 0 : 1; memcpy(f.rival[0], null_hash, PD0_HASH); f.eps_micro = o->eps_above_bound ? 50000 : 20000; f.min_trials = 5;
    b_append(b, LEDG_FALSIFIER, pl, (uint32_t)pd0_fals_write(&f, pl));
    /* 6. five preregistered experiments, each followed by its trial */
    pd0_exp *x = calloc(1, sizeof *x); uint8_t *xb = malloc(16384);
    for (int t = 0; t < 5; t++) { memset(x, 0, sizeof *x); x->n_obs = 2; x->n_hyp = 2; x->n_steps = 20;
        if (o->reuse_schedule && t == 0) { const pd0_rec *r0 = &b->recs[0]; x->reset[0] = r0->after[0]; x->reset[1] = r0->after[1]; for (int s = 0; s < 20; s++) { x->steps[s].channel = b->recs[1 + s].channel; x->steps[s].value = b->recs[1 + s].applied; } }
        else b_random_schedule(b, x->reset, x->steps, 20);
        { int64_t tmp[40]; pd0_rel_rollout_fn(&cand, x->reset, x->steps, 20, tmp); for (int s = 0; s < 20; s++) for (int j = 0; j < 2; j++) x->expected[0][s][j] = tmp[s * 2 + j]; pd0_rel_rollout_fn(&nullr, x->reset, x->steps, 20, tmp); for (int s = 0; s < 20; s++) for (int j = 0; j < 2; j++) x->expected[1][s][j] = tmp[s * 2 + j]; } x->divergence_micro = 5000000;
        size_t xn = pd0_exp_write(x, xb, 16384);
        if (!(o->trial_without_prereg && t == 4)) b_append(b, LEDG_PREREG, xb, (uint32_t)xn);
        if (o->stop_after_prereg) continue;
        b_tag(b, b->episode, TAG_TRIAL, 0, 0); b_episode(b, x->reset, x->steps, 20, w); }
    free(x); free(xb);
    if (o->stop_after_prereg || o->stop_after_trials) goto done;
    /* 7. three replication batches of 10 episodes */
    for (uint32_t bt = 0; bt < 3; bt++) { pd0_batch bb = { bt + 1, o->batch_seed_reuse ? 1000u : 1000u + bt, 10 }; b_append(b, LEDG_BATCH, pl, (uint32_t)pd0_batch_write(&bb, pl));
        for (int e = 0; e < 10; e++) { b_tag(b, b->episode, TAG_REP, bt + 1, o->all_planner ? ORIGIN_PLANNER : (e & 1) ? ORIGIN_RANDOM : ORIGIN_PLANNER); b_random_schedule(b, reset, steps, 20); b_episode(b, reset, steps, 20, w); } }
    if (o->demote_after_law) { null_w.k = w->k * 2; pd0_batch bb = { 9, 7777, 1 }; b_append(b, LEDG_BATCH, pl, (uint32_t)pd0_batch_write(&bb, pl)); b_tag(b, b->episode, TAG_REP, 9, ORIGIN_RANDOM); b_random_schedule(b, reset, steps, 20); b_episode(b, reset, steps, 20, &null_w); }
done:
    return pd0_ladder_check(b->buf, b->len, &b->P, R);
}
#endif
