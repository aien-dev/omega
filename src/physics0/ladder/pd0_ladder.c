#include "pd0_ladder.h"
#include "pd0_rng.h"
#include "sha256.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REC 65536u
#define MAX_EP 2048u
#define MAX_RELS 64u
#define MAX_PREREG 64u
#define MAX_BATCH 32u
#define EXP_BYTES 8192u

const char *pd0_ladder_state_name(int s)
{
    static const char *n[] = { "START", "OBSERVATION", "CORRELATION", "CANDIDATE", "HYPOTHESIS", "PREDICTED", "INTERVENED", "REPLICATED" };
    return (s >= 0 && s <= LS_REPLICATED) ? n[s] : "?";
}

void pd0_ladder_params_default(pd0_ladder_params *P, uint8_t n_obs, uint8_t n_channels)
{
    memset(P, 0, sizeof *P);
    P->n_obs = n_obs; P->n_channels = n_channels;
    for (int c = 0; c < n_channels; c++) { P->chan_min[c] = -2 * PD0_MICRO; P->chan_max[c] = 2 * PD0_MICRO; }
    P->reset_min = -2 * PD0_MICRO; P->reset_max = 2 * PD0_MICRO; P->episode_len = 100; P->dt_micro = 50000;
    P->size_bound = 6; P->eps_bound_micro = 20000;
    P->min_t1_records = 100; P->min_t1_episodes = 5; P->min_prereg = 5; P->min_batches = 3; P->min_batch_episodes = 10; P->rollout_steps = 20;
}

/* ---------- statistics ---------- */
double pd0_pearson(const double *x, const double *y, uint32_t n)
{
    double sx = 0, sy = 0; for (uint32_t i = 0; i < n; i++) { sx += x[i]; sy += y[i]; }
    double mx = sx / n, my = sy / n, sxy = 0, sxx = 0, syy = 0;
    for (uint32_t i = 0; i < n; i++) { double a = x[i] - mx, b = y[i] - my; sxy += a * b; sxx += a * a; syy += b * b; }
    if (sxx <= 0 || syy <= 0) return 0;
    return sxy / sqrt(sxx * syy);
}
double pd0_perm_p(const double *x, const double *y, uint32_t n, uint32_t n_shuffles, uint64_t seed)
{
    double r0 = fabs(pd0_pearson(x, y, n)); uint32_t hits = 0;
    double *yp = malloc(sizeof(double) * n); if (!yp) return 1.0;
    memcpy(yp, y, sizeof(double) * n);
    pd0_rng g; pd0_rng_stream(&g, seed, "perm");
    for (uint32_t s = 0; s < n_shuffles; s++) {
        for (uint32_t i = n - 1; i > 0; i--) { uint32_t j = (uint32_t)(pd0_rng_next(&g) % (i + 1)); double t = yp[i]; yp[i] = yp[j]; yp[j] = t; }
        if (fabs(pd0_pearson(x, yp, n)) >= r0) hits++;
    }
    free(yp);
    return (double)(hits + 1) / (double)(n_shuffles + 1);
}
typedef struct { double se[PD0_MAX_OBS], s[PD0_MAX_OBS], ss[PD0_MAX_OBS]; uint32_t n; } nrmse_acc;
static void acc_add(nrmse_acc *a, const int64_t *pred, const int64_t *obs, uint8_t n_obs)
{
    for (int j = 0; j < n_obs; j++) { double e = (double)(pred[j] - obs[j]) / 1e6, v = (double)obs[j] / 1e6; a->se[j] += e * e; a->s[j] += v; a->ss[j] += v * v; }
    a->n++;
}
static int64_t acc_nrmse(const nrmse_acc *a, uint8_t n_obs)
{
    if (a->n == 0) return INT64_MAX; double worst = 0;
    for (int j = 0; j < n_obs; j++) {
        double rmse = sqrt(a->se[j] / a->n), m = a->s[j] / a->n, var = a->ss[j] / a->n - m * m, sd = var > 0 ? sqrt(var) : 0;
        double v = sd > 1e-9 ? rmse / sd : rmse; if (v > worst) worst = v;
    }
    return worst > 9e12 ? INT64_MAX : (int64_t)(worst * 1e6);
}
int64_t pd0_nrmse_micro(const int64_t *pred, const int64_t *obs, uint32_t n, uint8_t n_obs)
{
    nrmse_acc a; memset(&a, 0, sizeof a);
    for (uint32_t i = 0; i < n; i++) acc_add(&a, pred + (size_t)i * n_obs, obs + (size_t)i * n_obs, n_obs);
    return acc_nrmse(&a, n_obs);
}
uint32_t pd0_confidence_ppm(uint32_t p, uint32_t f) { return (uint32_t)((1000000ull * (p + 1)) / (p + f + 2)); }

/* ---------- internal model ---------- */
typedef struct { uint32_t first, n; uint8_t tag, origin, has_tag; uint32_t batch_id, tag_entry, first_entry, episode_no; int complete; } episode;
typedef struct { uint8_t hash[PD0_HASH]; uint8_t role; pd0_rel rel; } relent;
typedef struct { pd0_exp e; uint8_t hash[PD0_HASH]; uint32_t entry; int matched_ep; } prereg;
typedef struct { pd0_batch b; uint8_t hash[PD0_HASH]; uint32_t entry; } batchent;

typedef struct {
    const pd0_ladder_params *P; pd0_ladder_report *R;
    pd0_rec *rec; uint32_t nrec; uint8_t last_rec_hash[PD0_HASH]; int have_rec;
    episode ep[MAX_EP]; uint32_t nep;
    relent rels[MAX_RELS]; uint32_t nrels;
    int have_fals; pd0_falsifier fals; uint8_t fals_hash[PD0_HASH];
    prereg pre[MAX_PREREG]; uint32_t npre;
    batchent bat[MAX_BATCH]; uint32_t nbat;
    uint32_t entry_idx;
} ctx;

static episode *find_ep(ctx *c, uint32_t no, int create)
{
    for (uint32_t i = 0; i < c->nep; i++) if (c->ep[i].episode_no == no) return &c->ep[i];
    if (!create || c->nep >= MAX_EP) return NULL;
    episode *e = &c->ep[c->nep++]; memset(e, 0, sizeof *e); e->episode_no = no; e->first = UINT32_MAX; return e;
}
static int ep_visible(const episode *e) { return e->has_tag && (e->tag == TAG_FIT || e->tag == TAG_SELECT); }
static int ep_fit(const episode *e) { return e->has_tag && e->tag == TAG_FIT; }
static uint32_t ep_ok_steps(const ctx *c, const episode *e)
{
    uint32_t k = 0; for (uint32_t i = 0; i < e->n; i++) { const pd0_rec *r = &c->rec[e->first + i]; if (r->kind == PD0_KIND_STEP && r->status == PD0_ST_OK) k++; }
    return k;
}
/* schedule hash over the reset record and the first n_steps OK steps */
static int ep_schedule_hash(const ctx *c, const episode *e, uint8_t n_steps, uint8_t out[PD0_HASH])
{
    const pd0_rec *r0 = &c->rec[e->first]; if (r0->kind != PD0_KIND_RESET) return 0;
    pd0_step st[PD0_MAX_STEPS]; uint8_t k = 0;
    for (uint32_t i = 1; i < e->n && k < n_steps; i++) { const pd0_rec *r = &c->rec[e->first + i]; if (r->kind == PD0_KIND_STEP && r->status == PD0_ST_OK) { st[k].channel = r->channel; st[k].value = r->applied; k++; } }
    if (k < n_steps) return 0;
    pd0_schedule_hash(r0->n_obs, r0->after, n_steps, st, out);
    return 1;
}
/* spec rev 5: a preregistered schedule binds the REQUESTED reset values and the applied (channel, value)
 * steps, never the observed reset (which carries noise on noisy levels). The requested reset is the one
 * in the PD0EXP1 entry; the episode matches when the hash of that reset with the episode's applied steps
 * equals the preregistered hash and, on noise-free levels, the observed reset equals the requested one.
 * On noisy levels the observed reset is not compared (limit: the trial error bound covers it). */
static int ep_matches_prereg(const ctx *c, const episode *ep, const pd0_exp *e)
{
    const pd0_rec *r0 = &c->rec[ep->first]; if (r0->kind != PD0_KIND_RESET || r0->n_obs != e->n_obs) return 0;
    pd0_step st[PD0_MAX_STEPS]; uint8_t k = 0;
    for (uint32_t i = 1; i < ep->n && k < e->n_steps; i++) { const pd0_rec *r = &c->rec[ep->first + i]; if (r->kind == PD0_KIND_STEP && r->status == PD0_ST_OK) { st[k].channel = r->channel; st[k].value = r->applied; k++; } }
    if (k < e->n_steps) return 0;
    uint8_t h[PD0_HASH]; pd0_schedule_hash(e->n_obs, e->reset, e->n_steps, st, h); if (memcmp(h, e->schedule_hash, PD0_HASH)) return 0;
    if (!c->P->noisy) for (int j = 0; j < e->n_obs; j++) if (r0->after[j] != e->reset[j]) return 0;
    return 1;
}
static void set_code(ctx *c, int t, int code) { c->R->transition = t; c->R->code = code; if (code == PD0V_OK) c->R->stall_code = 0; else if (!c->R->stall_code) c->R->stall_code = code; }
static void refute(ctx *c, const pd0_rec *bad, int64_t predicted, int64_t observed)
{
    pd0_ladder_report *R = c->R;
    if (R->n_exceptions < PD0_MAX_EXC) {
        pd0_exception *x = &R->exc[R->n_exceptions++]; x->record_seq = bad->seq; memcpy(x->record_hash, bad->record_hash, PD0_HASH);
        x->predicted = predicted; x->observed = observed; x->error_micro = predicted - observed;
    }
    R->n_refutations++;
    R->have_candidate = 0; c->have_fals = 0; c->npre = 0; c->nbat = 0;
    for (uint32_t i = 0; i < c->nep; i++) if (c->ep[i].has_tag) { c->ep[i].tag = TAG_FIT; }
    R->state = LS_CANDIDATE;
}
static void add_experiment(ctx *c, const uint8_t *id, uint8_t kind, const uint8_t *prereg_hash, const uint8_t *outcome, uint8_t result, uint64_t first, uint64_t last)
{
    pd0_ladder_report *R = c->R; if (R->n_experiments >= PD0_MAX_EXP) return;
    pd0_experiment *x = &R->exp[R->n_experiments++]; memcpy(x->experiment_id, id, PD0_HASH); x->kind = kind;
    memcpy(x->prereg_hash, prereg_hash, PD0_HASH); memcpy(x->outcome_hash, outcome, PD0_HASH); x->result = result; x->first_seq = first; x->last_seq = last;
}

/* rollout of relation along an episode's recorded applied schedule; fills pred[n][n_obs] for the first n OK steps */
static uint32_t rollout_episode(const ctx *c, const pd0_rel *rel, const episode *e, uint32_t n, int64_t *pred, int64_t *obs, const pd0_rec **last)
{
    const pd0_rec *r0 = &c->rec[e->first]; int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; uint32_t k = 0; uint8_t no = c->P->n_obs;
    memset(st, 0, sizeof st); for (int j = 0; j < no; j++) st[j] = r0->after[j];
    for (uint32_t i = 1; i < e->n && k < n; i++) {
        const pd0_rec *r = &c->rec[e->first + i]; if (r->kind != PD0_KIND_STEP || r->status != PD0_ST_OK) continue;
        pd0_rel_step(rel, st, r->channel, r->applied, nx);
        for (int j = 0; j < no; j++) { pred[k * no + j] = nx[j]; obs[k * no + j] = r->after[j]; }
        memcpy(st, nx, sizeof st); k++; if (last) *last = r;
    }
    return k;
}

/* ---------- T1 ---------- */
static void try_t1(ctx *c)
{
    uint32_t ok = 0, eps = 0;
    for (uint32_t i = 0; i < c->nep; i++) { const episode *e = &c->ep[i]; if (!ep_visible(e)) continue; uint32_t k = ep_ok_steps(c, e); if (k) { ok += k; eps++; } }
    if (ok < c->P->min_t1_records) { set_code(c, 1, PD0V_T1_TOO_FEW_RECORDS); return; }
    if (eps < c->P->min_t1_episodes) { set_code(c, 1, PD0V_T1_TOO_FEW_EPISODES); return; }
    c->R->state = LS_OBSERVATION; set_code(c, 1, PD0V_OK);
}
/* ---------- T2 ---------- */
static int try_t2(ctx *c, const pd0_corr *k)
{
    uint8_t no = c->P->n_obs; if (k->var_b >= no || (k->var_a >= no && k->var_a != PD0_CHAN_NONE)) return PD0V_BAD_FIELD;
    if (k->var_a == k->var_b) { set_code(c, 2, PD0V_T2_NO_EVIDENCE); return 0; }   /* spec rev 5: a variable against its own one-step change is regression to the mean, not evidence */
    if (k->n_shuffles < 2000) { set_code(c, 2, PD0V_T2_TOO_FEW_SHUFFLES); return 0; }
    double *x = malloc(sizeof(double) * MAX_REC), *y = malloc(sizeof(double) * MAX_REC); uint32_t n = 0;
    if (!x || !y) { free(x); free(y); return PD0V_TOO_LARGE; }
    for (uint32_t i = 0; i < c->nep; i++) { const episode *e = &c->ep[i]; if (!ep_visible(e)) continue;
        for (uint32_t j = 0; j < e->n; j++) { const pd0_rec *r = &c->rec[e->first + j]; if (r->kind != PD0_KIND_STEP || r->status != PD0_ST_OK) continue;
            x[n] = (k->var_a == PD0_CHAN_NONE) ? (double)r->applied : (double)r->before[k->var_a]; y[n] = (double)(r->after[k->var_b] - r->before[k->var_b]); n++; } }
    int rc = 0;
    if (n < 100 || n != k->n) { set_code(c, 2, PD0V_T2_TOO_FEW_RECORDS); }
    else {
        double r = pd0_pearson(x, y, n); c->R->last_r = r;
        if (llabs((int64_t)(r * 1e6) - k->r_micro) > 1000) { set_code(c, 2, PD0V_T2_CORR_MISMATCH); rc = PD0V_T2_CORR_MISMATCH; }
        else if (fabs(r) < 0.3) set_code(c, 2, PD0V_T2_CORR_TOO_WEAK);
        else {
            double p = pd0_perm_p(x, y, n, k->n_shuffles, k->shuffle_seed) * (k->n_pairs ? k->n_pairs : 1); c->R->last_p = p;
            if (p > 0.01 || k->p_micro > 10000) set_code(c, 2, PD0V_T2_P_TOO_HIGH);
            else { c->R->state = LS_CORRELATION; set_code(c, 2, PD0V_OK); }
        }
    }
    free(x); free(y); return rc;
}
/* ---------- T3 ---------- */
static int rep_exists(const ctx *c) { for (uint32_t i = 0; i < c->nep; i++) if (c->ep[i].has_tag && c->ep[i].tag == TAG_REP) return 1; return 0; }
static void try_t3(ctx *c, const relent *re)
{
    const pd0_ladder_params *P = c->P; const pd0_rel *rel = &re->rel; uint8_t no = P->n_obs;
    if (rel->n_vars != no + rel->n_latent || rel->n_channels != P->n_channels) { set_code(c, 3, PD0V_T3_BAD_RELATION); return; }
    if (pd0_rel_max_degree(rel) > 3) { set_code(c, 3, PD0V_T3_DEGREE_EXCEEDED); return; }
    if (pd0_rel_size(rel) > P->size_bound) { set_code(c, 3, PD0V_T3_SIZE_EXCEEDED); return; }
    /* one-step error on FIT with teacher forcing on observed variables */
    nrmse_acc a; memset(&a, 0, sizeof a); double lag_num[PD0_MAX_OBS] = { 0 }, lag_den[PD0_MAX_OBS] = { 0 };
    for (uint32_t i = 0; i < c->nep; i++) { const episode *e = &c->ep[i]; if (!ep_fit(e)) continue;
        int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; memset(st, 0, sizeof st); double prev_res[PD0_MAX_OBS] = { 0 }; int have_prev = 0;
        for (uint32_t j = 0; j < e->n; j++) { const pd0_rec *r = &c->rec[e->first + j];
            if (r->kind == PD0_KIND_RESET) { memset(st, 0, sizeof st); for (int v = 0; v < no; v++) st[v] = r->after[v]; have_prev = 0; continue; }
            if (r->status != PD0_ST_OK) continue;
            for (int v = 0; v < no; v++) st[v] = r->before[v];
            pd0_rel_step(rel, st, r->channel, r->applied, nx);
            acc_add(&a, nx, r->after, no);
            for (int v = 0; v < no; v++) { double res = (double)(r->after[v] - nx[v]) / 1e6; if (have_prev) { lag_num[v] += res * prev_res[v]; } lag_den[v] += res * res; prev_res[v] = res; }
            have_prev = 1; memcpy(st, nx, sizeof st); } }
    int64_t one = acc_nrmse(&a, no); c->R->last_nrmse_micro = one;
    if (!P->noisy && one > 50000) { set_code(c, 3, PD0V_T3_FIT_ERROR_TOO_HIGH); return; }
    if (P->noisy) { for (int v = 0; v < no; v++) { double ac = lag_den[v] > 0 ? lag_num[v] / lag_den[v] : 0; if (fabs(ac) > 0.1) { set_code(c, 3, PD0V_T3_RESIDUALS_NOT_WHITE); return; } } }
    c->R->have_candidate = 1; c->R->candidate = *rel; memcpy(c->R->candidate_hash, re->hash, PD0_HASH);
    c->R->state = LS_CANDIDATE; set_code(c, 3, PD0V_OK);
}
/* ---------- T4 ---------- */
static const relent *find_rel(const ctx *c, const uint8_t *h) { for (uint32_t i = 0; i < c->nrels; i++) if (!memcmp(c->rels[i].hash, h, PD0_HASH)) return &c->rels[i]; return NULL; }
static int try_t4(ctx *c, const pd0_falsifier *f, const uint8_t *entry_hash)
{
    if (memcmp(f->candidate, c->R->candidate_hash, PD0_HASH) != 0) { set_code(c, 4, PD0V_T4_UNKNOWN_CANDIDATE); return PD0V_T4_UNKNOWN_CANDIDATE; }
    if (f->n_rivals == 0) { set_code(c, 4, PD0V_T4_NO_RIVAL); return 0; }
    for (int k = 0; k < f->n_rivals; k++) { const relent *r = find_rel(c, f->rival[k]); if (!r || r->role == REL_CANDIDATE || !memcmp(f->rival[k], f->candidate, PD0_HASH)) { set_code(c, 4, PD0V_T4_UNKNOWN_RIVAL); return PD0V_T4_UNKNOWN_RIVAL; } }
    if (f->eps_micro <= 0 || f->eps_micro > c->P->eps_bound_micro) { set_code(c, 4, PD0V_T4_EPS_ABOVE_BOUND); return 0; }
    for (uint32_t i = 0; i < c->nep; i++) if (c->ep[i].has_tag && c->ep[i].tag == TAG_TRIAL) { set_code(c, 4, PD0V_T4_FALSIFIER_AFTER_TRIAL); return PD0V_T4_FALSIFIER_AFTER_TRIAL; }
    c->have_fals = 1; c->fals = *f; memcpy(c->fals_hash, entry_hash, PD0_HASH); c->R->eps_micro = f->eps_micro;
    c->R->state = LS_HYPOTHESIS; set_code(c, 4, PD0V_OK); return 0;
}
/* ---------- T5 ---------- */
static int try_t5(ctx *c, const pd0_exp *e, const uint8_t *entry_hash)
{
    const pd0_ladder_params *P = c->P;
    if (e->n_obs != P->n_obs || e->n_steps != P->rollout_steps) return PD0V_BAD_FIELD;
    if (e->n_hyp != 1u + c->fals.n_rivals) { set_code(c, 5, PD0V_T5_MISSING_RIVAL_PREDICTION); return PD0V_T5_MISSING_RIVAL_PREDICTION; }
    for (int j = 0; j < e->n_obs; j++) if (e->reset[j] < P->reset_min || e->reset[j] > P->reset_max) { set_code(c, 5, PD0V_T5_OUT_OF_BOUNDS); return PD0V_T5_OUT_OF_BOUNDS; }
    for (int s = 0; s < e->n_steps; s++) { uint8_t ch = e->steps[s].channel; if (ch != PD0_CHAN_NONE && (ch >= P->n_channels || e->steps[s].value < P->chan_min[ch] || e->steps[s].value > P->chan_max[ch])) { set_code(c, 5, PD0V_T5_OUT_OF_BOUNDS); return PD0V_T5_OUT_OF_BOUNDS; } }
    for (uint32_t i = 0; i < c->nep; i++) { const episode *ep = &c->ep[i]; if (!ep->has_tag || ep->tag > TAG_HOLDOUT || ep->n == 0) continue;
        if (ep_matches_prereg(c, ep, e)) { set_code(c, 5, PD0V_T5_SCHEDULE_REUSED); return PD0V_T5_SCHEDULE_REUSED; } }
    for (uint32_t i = 0; i < c->npre; i++) if (!memcmp(c->pre[i].e.schedule_hash, e->schedule_hash, PD0_HASH)) { set_code(c, 5, PD0V_T5_SCHEDULE_REUSED); return PD0V_T5_SCHEDULE_REUSED; }
    if (c->npre >= MAX_PREREG) return PD0V_TOO_LARGE;
    prereg *p = &c->pre[c->npre++]; p->e = *e; memcpy(p->hash, entry_hash, PD0_HASH); p->entry = c->entry_idx; p->matched_ep = -1;
    uint32_t need = c->fals.min_trials > P->min_prereg ? c->fals.min_trials : P->min_prereg;
    if (c->npre >= need) { c->R->state = LS_PREDICTED; set_code(c, 5, PD0V_OK); } else set_code(c, 5, PD0V_T5_TOO_FEW_PREREG);
    return 0;
}
/* ---------- T6 ---------- */
static int try_t6(ctx *c)
{
    const pd0_ladder_params *P = c->P; uint8_t no = P->n_obs; uint32_t n = P->rollout_steps;
    /* match complete TRIAL episodes to preregistrations */
    for (uint32_t i = 0; i < c->nep; i++) { episode *ep = &c->ep[i]; if (!ep->has_tag || ep->tag != TAG_TRIAL || ep->n == 0) continue;
        if (ep_ok_steps(c, ep) < n) continue;
        int found = 0;
        for (uint32_t k = 0; k < c->npre; k++) if (c->pre[k].entry < ep->first_entry && ep_matches_prereg(c, ep, &c->pre[k].e)) { c->pre[k].matched_ep = (int)i; found = 1; break; }
        if (!found) { set_code(c, 6, PD0V_T6_TRIAL_NOT_PREREGISTERED); return PD0V_T6_TRIAL_NOT_PREREGISTERED; } }
    for (uint32_t k = 0; k < c->npre; k++) if (c->pre[k].matched_ep < 0) { set_code(c, 6, PD0V_T6_TRIAL_MISSING); return 0; }
    /* evaluate every trial against the committed prediction (hypothesis index 0) */
    int64_t obs[PD0_MAX_STEPS * PD0_MAX_OBS], pk[PD0_MAX_STEPS * PD0_MAX_OBS];
    for (uint32_t k = 0; k < c->npre; k++) { prereg *p = &c->pre[k]; const episode *ep = &c->ep[p->matched_ep]; uint32_t m = 0; const pd0_rec *last = NULL, *worst_rec = NULL; int64_t wp = 0, wo = 0, wd = -1;
        for (uint32_t i = 1; i < ep->n && m < n; i++) { const pd0_rec *r = &c->rec[ep->first + i]; if (r->kind != PD0_KIND_STEP || r->status != PD0_ST_OK) continue;
            for (int j = 0; j < no; j++) { obs[m * no + j] = r->after[j]; int64_t d = llabs(p->e.expected[0][m][j] - r->after[j]); if (d > wd) { wd = d; worst_rec = r; wp = p->e.expected[0][m][j]; wo = r->after[j]; } }
            m++; last = r; }
        for (uint32_t s = 0; s < m; s++) for (int j = 0; j < no; j++) pk[s * no + j] = p->e.expected[0][s][j];
        int64_t err = pd0_nrmse_micro(pk, obs, m, no); c->R->last_nrmse_micro = err;
        uint8_t id[PD0_HASH]; sha256_hash(p->hash, PD0_HASH, id);
        if (err > c->R->eps_micro) { c->R->f++; add_experiment(c, id, 0, p->hash, last->record_hash, 1, c->rec[ep->first].seq, last->seq); refute(c, worst_rec, wp, wo); set_code(c, 6, PD0V_T6_TRIAL_FAILED); return 0; }
        c->R->p++; add_experiment(c, id, 0, p->hash, last->record_hash, 0, c->rec[ep->first].seq, last->seq); }
    c->R->state = LS_INTERVENED; set_code(c, 6, PD0V_OK); return 0;
}
/* ---------- T7 ---------- */
static int try_t7(ctx *c)
{
    const pd0_ladder_params *P = c->P; uint8_t no = P->n_obs; uint32_t n = P->rollout_steps; uint32_t ready = 0;
    int64_t pred[PD0_MAX_STEPS * PD0_MAX_OBS], obs[PD0_MAX_STEPS * PD0_MAX_OBS];
    for (uint32_t b = 0; b < c->nbat; b++) { uint32_t cnt = 0, planner = 0, random = 0; for (uint32_t i = 0; i < c->nep; i++) { const episode *ep = &c->ep[i]; if (ep->has_tag && ep->tag == TAG_REP && ep->batch_id == c->bat[b].b.batch_id && ep_ok_steps(c, ep) >= n) { cnt++; if (ep->origin == ORIGIN_PLANNER) planner++; else random++; } }
        if (cnt < P->min_batch_episodes || cnt < c->bat[b].b.n_episodes) continue;
        if (planner + 1 < random || random + 1 < planner) { set_code(c, 7, PD0V_T7_ORIGIN_IMBALANCE); return PD0V_T7_ORIGIN_IMBALANCE; }
        ready++; }
    if (ready < P->min_batches) { set_code(c, 7, ready || c->nbat ? PD0V_T7_BATCH_TOO_SMALL : PD0V_T7_TOO_FEW_BATCHES); return 0; }
    for (uint32_t b = 0; b < c->nbat; b++) { nrmse_acc a; memset(&a, 0, sizeof a); const pd0_rec *first = NULL, *last = NULL;
        for (uint32_t i = 0; i < c->nep; i++) { const episode *ep = &c->ep[i]; if (!(ep->has_tag && ep->tag == TAG_REP && ep->batch_id == c->bat[b].b.batch_id && ep_ok_steps(c, ep) >= n)) continue;
            /* freshness: schedule must not equal any other episode's schedule */
            uint8_t h1[PD0_HASH], h2[PD0_HASH]; ep_schedule_hash(c, ep, (uint8_t)n, h1);
            for (uint32_t j = 0; j < c->nep; j++) if (j != i && c->ep[j].n && ep_schedule_hash(c, &c->ep[j], (uint8_t)n, h2) && !memcmp(h1, h2, PD0_HASH)) { set_code(c, 7, PD0V_T7_STREAM_REUSED); return PD0V_T7_STREAM_REUSED; }
            const pd0_rec *l = NULL; uint32_t m = rollout_episode(c, &c->R->candidate, ep, n, pred, obs, &l); if (!first) first = &c->rec[ep->first]; last = l;
            int64_t e1 = pd0_nrmse_micro(pred, obs, m, no);
            if (e1 > 3 * c->R->eps_micro) { c->R->f++; uint8_t id[PD0_HASH]; sha256_hash(c->bat[b].hash, PD0_HASH, id); add_experiment(c, id, 1, c->bat[b].hash, l->record_hash, 1, first->seq, l->seq); refute(c, l, pred[(m - 1) * no], obs[(m - 1) * no]); set_code(c, 7, PD0V_T7_EPISODE_ABOVE_3EPS); return 0; }
            for (uint32_t s = 0; s < m; s++) acc_add(&a, pred + s * no, obs + s * no, no); c->R->p++; }
        int64_t eb = acc_nrmse(&a, no); c->R->last_nrmse_micro = eb; uint8_t id[PD0_HASH]; sha256_hash(c->bat[b].hash, PD0_HASH, id);
        if (eb > c->R->eps_micro) { c->R->f++; add_experiment(c, id, 1, c->bat[b].hash, last->record_hash, 1, first->seq, last->seq); refute(c, last, pred[(n - 1) * no], obs[(n - 1) * no]); set_code(c, 7, PD0V_T7_BATCH_FAILED); return 0; }
        add_experiment(c, id, 1, c->bat[b].hash, last->record_hash, 0, first->seq, last->seq); }
    c->R->state = LS_REPLICATED; set_code(c, 7, PD0V_OK); return 0;
}
/* ---------- post-law contradiction (demotion) ---------- */
static void check_demotion(ctx *c, episode *ep)
{
    if (c->R->state != LS_REPLICATED || !(ep->has_tag && (ep->tag == TAG_TRIAL || ep->tag == TAG_REP)) || ep->complete) return;
    uint32_t n = c->P->rollout_steps; if (ep_ok_steps(c, ep) < n) return; ep->complete = 1;
    int64_t pred[PD0_MAX_STEPS * PD0_MAX_OBS], obs[PD0_MAX_STEPS * PD0_MAX_OBS]; const pd0_rec *l = NULL;
    uint32_t m = rollout_episode(c, &c->R->candidate, ep, n, pred, obs, &l);
    if (pd0_nrmse_micro(pred, obs, m, c->P->n_obs) > c->R->eps_micro) { c->R->f++; refute(c, l, pred[(m - 1) * c->P->n_obs], obs[(m - 1) * c->P->n_obs]); c->R->state = LS_HYPOTHESIS; c->R->demoted = 1; c->R->have_candidate = 1; set_code(c, 8, PD0V_LAW_DEMOTED); }
    else c->R->p++;
}

static void compute_domain(ctx *c)
{
    pd0_domain *d = &c->R->dom; const pd0_ladder_params *P = c->P; memset(d, 0, sizeof *d);
    d->n_obs = P->n_obs; d->n_channels = P->n_channels; d->dt_micro = P->dt_micro; d->reset_min = P->reset_min; d->reset_max = P->reset_max; d->episode_len = P->episode_len;
    d->latent_reset = 0; int first = 1;
    for (uint32_t i = 0; i < c->nep; i++) { const episode *e = &c->ep[i]; if (!e->has_tag || e->tag == TAG_HOLDOUT || e->tag == TAG_REP || e->n == 0) continue; int used = 0;
        for (uint32_t j = 0; j < e->n; j++) { const pd0_rec *r = &c->rec[e->first + j]; if (r->status != PD0_ST_OK) continue; used = 1; d->n_observations++;
            for (int v = 0; v < P->n_obs; v++) { int64_t lo = r->before[v] < r->after[v] ? r->before[v] : r->after[v], hi = r->before[v] > r->after[v] ? r->before[v] : r->after[v];
                if (first || lo < d->var_min[v]) d->var_min[v] = lo; if (first || hi > d->var_max[v]) d->var_max[v] = hi; }
            if (r->kind == PD0_KIND_STEP && r->channel < P->n_channels) { uint8_t ch = r->channel; if (d->chan_min[ch] == 0 && d->chan_max[ch] == 0) { d->chan_min[ch] = d->chan_max[ch] = r->applied; } if (r->applied < d->chan_min[ch]) d->chan_min[ch] = r->applied; if (r->applied > d->chan_max[ch]) d->chan_max[ch] = r->applied; }
            first = 0; }
        if (used) d->n_episodes++; }
}

int pd0_ladder_check(const uint8_t *ledger, size_t len, const pd0_ladder_params *P, pd0_ladder_report *R)
{
    ctx *c = calloc(1, sizeof *c); if (!c) return PD0V_TOO_LARGE;
    c->rec = malloc(sizeof(pd0_rec) * MAX_REC); if (!c->rec) { free(c); return PD0V_TOO_LARGE; }
    memset(R, 0, sizeof *R); c->P = P; c->R = R; R->state = LS_START; R->code = PD0V_T1_TOO_FEW_RECORDS; R->transition = 1;
    uint8_t prev[PD0_HASH] = { 0 }; size_t off = 0; int rc = 0;
    while (off < len) {
        pd0_entry e; rc = pd0_ledg_next(ledger, len, &off, prev, &e); if (rc) break;
        memcpy(prev, e.entry_hash, PD0_HASH); memcpy(R->chain_root, e.entry_hash, PD0_HASH); R->n_entries++; c->entry_idx++;
        switch (e.kind) {
        case LEDG_OBS: {
            if (c->nrec >= MAX_REC) { rc = PD0V_TOO_LARGE; break; }
            pd0_rec *r = &c->rec[c->nrec]; size_t used; rc = pd0_rec_parse(e.payload, e.payload_len, r, &used); if (rc) break;
            if (used != e.payload_len || r->n_obs != P->n_obs) { rc = PD0V_BAD_FIELD; break; }
            if (c->have_rec) { if (memcmp(r->prev_hash, c->last_rec_hash, PD0_HASH) || r->seq != c->rec[c->nrec - 1].seq + 1) { rc = PD0V_CHAIN_BROKEN; break; } }
            else { uint8_t z[PD0_HASH] = { 0 }; if (memcmp(r->prev_hash, z, PD0_HASH) || r->seq != 0) { rc = PD0V_CHAIN_BROKEN; break; } }
            memcpy(c->last_rec_hash, r->record_hash, PD0_HASH); c->have_rec = 1;
            episode *ep = find_ep(c, r->episode, 1); if (!ep) { rc = PD0V_TOO_LARGE; break; }
            if (ep->first == UINT32_MAX) { ep->first = c->nrec; ep->first_entry = c->entry_idx; if (r->kind != PD0_KIND_RESET) { rc = PD0V_BAD_FIELD; break; } }
            else if (ep->first + ep->n != c->nrec) { rc = PD0V_BAD_FIELD; break; } /* episodes are contiguous */
            ep->n++; c->nrec++; R->n_records++;
            if (ep->has_tag && ep->tag == TAG_TRIAL && ep_ok_steps(c, ep) == P->rollout_steps) { int found = 0; for (uint32_t k = 0; k < c->npre; k++) if (c->pre[k].entry < ep->first_entry && ep_matches_prereg(c, ep, &c->pre[k].e)) found = 1;
                if (!found) { rc = PD0V_T6_TRIAL_NOT_PREREGISTERED; set_code(c, 6, rc); break; } }
            if (ep->has_tag && (ep->tag == TAG_TRIAL || ep->tag == TAG_REP) && ep->tag_entry > ep->first_entry) { rc = PD0V_T6_TRIAL_NOT_PREREGISTERED; break; }
            if (R->state == LS_START) try_t1(c);
            else if (R->state == LS_PREDICTED) rc = try_t6(c);
            else if (R->state == LS_INTERVENED) rc = try_t7(c);
            else if (R->state == LS_REPLICATED) check_demotion(c, ep);
            break; }
        case LEDG_TAG: { pd0_tag t; rc = pd0_tag_parse(e.payload, e.payload_len, &t); if (rc) break;
            episode *ep = find_ep(c, t.episode, 1); if (!ep) { rc = PD0V_TOO_LARGE; break; }
            if ((t.tag == TAG_TRIAL || t.tag == TAG_REP) && ep->n) { rc = PD0V_T6_TRIAL_NOT_PREREGISTERED; break; }
            ep->has_tag = 1; ep->tag = t.tag; ep->origin = t.origin; ep->batch_id = t.batch_id; ep->tag_entry = c->entry_idx;
            if (R->state == LS_START) try_t1(c);
            break; }
        case LEDG_CORR: { pd0_corr k; rc = pd0_corr_parse(e.payload, e.payload_len, &k); if (rc) break;
            if (R->state != LS_OBSERVATION) { if (R->state < LS_OBSERVATION) { rc = PD0V_T8_STATE_SKIPPED; set_code(c, 2, rc); } break; }
            rc = try_t2(c, &k); break; }
        case LEDG_RELATION: { if (c->nrels >= MAX_RELS) { rc = PD0V_TOO_LARGE; break; } relent *re = &c->rels[c->nrels]; size_t used;
            if (e.payload_len < 1) { rc = PD0V_TRUNCATED; break; } re->role = e.payload[0];
            rc = pd0_rel_parse(e.payload + 1, e.payload_len - 1, &re->rel, &used); if (rc) break; if (used + 1 != e.payload_len) { rc = PD0V_BAD_FIELD; break; }
            memcpy(re->hash, e.entry_hash, PD0_HASH); c->nrels++;
            if (re->role == REL_CANDIDATE) { if (rep_exists(c)) { rc = PD0V_REP_INFLUENCED_FIT; set_code(c, 3, rc); break; }
                if (R->state == LS_CORRELATION || R->state == LS_CANDIDATE) try_t3(c, re);
                else if (R->state < LS_CORRELATION) { rc = PD0V_T8_STATE_SKIPPED; set_code(c, 3, rc); } }
            break; }
        case LEDG_FALSIFIER: { pd0_falsifier f; rc = pd0_fals_parse(e.payload, e.payload_len, &f); if (rc) break;
            if (R->state != LS_CANDIDATE) { if (R->state < LS_CANDIDATE) { rc = PD0V_T8_STATE_SKIPPED; set_code(c, 4, rc); } break; }
            rc = try_t4(c, &f, e.entry_hash); break; }
        case LEDG_PREREG: { pd0_exp x; size_t used; rc = pd0_exp_parse(e.payload, e.payload_len, &x, &used); if (rc) break;
            if (R->state != LS_HYPOTHESIS && R->state != LS_PREDICTED) { if (R->state < LS_HYPOTHESIS) { rc = PD0V_T8_STATE_SKIPPED; set_code(c, 5, rc); } break; }
            rc = try_t5(c, &x, e.entry_hash); break; }
        case LEDG_BATCH: { pd0_batch b; rc = pd0_batch_parse(e.payload, e.payload_len, &b); if (rc) break;
            if (R->state < LS_INTERVENED) { rc = PD0V_T8_STATE_SKIPPED; set_code(c, 7, rc); break; }
            for (uint32_t i = 0; i < c->nbat; i++) if (c->bat[i].b.stream_seed == b.stream_seed || c->bat[i].b.batch_id == b.batch_id) { rc = PD0V_T7_STREAM_REUSED; set_code(c, 7, rc); break; }
            if (rc) break; if (c->nbat >= MAX_BATCH) { rc = PD0V_TOO_LARGE; break; }
            c->bat[c->nbat].b = b; memcpy(c->bat[c->nbat].hash, e.entry_hash, PD0_HASH); c->bat[c->nbat].entry = c->entry_idx; c->nbat++;
            break; }
        default: rc = PD0V_BAD_FIELD;
        }
        if (rc) break;
    }
    if (!rc && R->state == LS_PREDICTED) { int r2 = try_t6(c); if (r2) rc = r2; }
    if (!rc && R->state == LS_INTERVENED) { int r2 = try_t7(c); if (r2) rc = r2; }
    compute_domain(c);
    R->confidence_ppm = pd0_confidence_ppm(R->p, R->f);
    if (rc) R->code = rc;
    free(c->rec); free(c);
    return rc;
}

/* ---------- claim text and T8 ---------- */
static void fmt_micro(char *o, size_t cap, int64_t v)
{
    int64_t a = v < 0 ? -v : v; snprintf(o, cap, "%s%lld.%06lld", v < 0 ? "-" : "", (long long)(a / PD0_MICRO), (long long)(a % PD0_MICRO));
}
uint32_t pd0_claim_text(const pd0_ladder_report *R, const pd0_ladder_params *P, char *out, size_t cap)
{
    char dom[256] = "", a[32], b[32], eps[32]; size_t dl = 0;
    for (int v = 0; v < P->n_obs; v++) { fmt_micro(a, sizeof a, R->dom.var_min[v]); fmt_micro(b, sizeof b, R->dom.var_max[v]); dl += (size_t)snprintf(dom + dl, sizeof dom - dl, "%ss%d in [%s, %s]", v ? "; " : "", v, a, b); if (dl >= sizeof dom) break; }
    fmt_micro(eps, sizeof eps, R->eps_micro);
    int n = snprintf(out, cap, "Across %llu observations under %s, this relation predicts within %s (rollout NRMSE, 20 steps). Confidence %u ppm. %u exceptions recorded.",
                     (unsigned long long)R->dom.n_observations, dom, eps, R->confidence_ppm, R->n_exceptions);
    return n < 0 ? 0 : (uint32_t)n;
}
static int has_forbidden(const uint8_t *s, uint32_t n)
{
    static const char *bad[] = { "always", "universally", "true", "Always", "Universally", "True" };
    char buf[PD0_MAX_CLAIM + 1]; if (n > PD0_MAX_CLAIM) return 1; memcpy(buf, s, n); buf[n] = 0;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) if (strstr(buf, bad[i])) return 1;
    return 0;
}
size_t pd0_ladder_emit_law(const pd0_ladder_report *R, const pd0_ladder_params *P, uint8_t *out, size_t cap)
{
    pd0_law *l = calloc(1, sizeof *l); if (!l) return 0;
    l->state = (R->state == LS_REPLICATED && !R->demoted) ? PDLAW_PROVISIONAL_LAW : (R->n_refutations && !R->have_candidate) ? PDLAW_REFUTED : R->have_candidate ? PDLAW_HYPOTHESIS : PDLAW_REJECTED;
    l->rel = R->candidate; l->rel.description_bits = pd0_rel_bits(&l->rel); l->rel.n_refutations = (uint16_t)R->n_refutations;
    l->dom = R->dom; l->confidence_ppm = R->confidence_ppm;
    l->n_exceptions = R->n_exceptions; memcpy(l->exc, R->exc, sizeof(pd0_exception) * R->n_exceptions);
    l->n_experiments = R->n_experiments; memcpy(l->exp, R->exp, sizeof(pd0_experiment) * R->n_experiments);
    memcpy(l->chain_root, R->chain_root, PD0_HASH);
    l->claim_len = pd0_claim_text(R, P, (char *)l->claim, sizeof l->claim);
    size_t n = pd0_law_write(l, out, cap); free(l); return n;
}
int pd0_ladder_verify_law(const pd0_ladder_report *R, const pd0_ladder_params *P, const uint8_t *law, size_t law_len)
{
    pd0_law *l = calloc(1, sizeof *l); if (!l) return PD0V_TOO_LARGE; int rc = pd0_law_parse(law, law_len, l);
    if (rc) { free(l); return rc; }
    if (l->state == PDLAW_PROVISIONAL_LAW) {
        if (R->demoted) rc = PD0V_LAW_DEMOTED;
        else if (R->state != LS_REPLICATED) rc = PD0V_T8_STATE_SKIPPED;
    } else if (l->state == PDLAW_HYPOTHESIS && !(R->state >= LS_HYPOTHESIS || R->demoted)) rc = PD0V_T8_STATE_MISMATCH;
    if (!rc && has_forbidden(l->claim, l->claim_len)) rc = PD0V_T8_CLAIM_FORBIDDEN_WORD;
    if (!rc && l->state != PDLAW_REJECTED) {
        pd0_rel want = R->candidate; want.description_bits = pd0_rel_bits(&want); want.n_refutations = (uint16_t)R->n_refutations;
        uint8_t a[4096], b[4096]; size_t na = pd0_rel_write(&want, a, sizeof a), nb = pd0_rel_write(&l->rel, b, sizeof b);
        if (l->rel.n_refutations != R->n_refutations) rc = PD0V_T8_REFUTATION_COUNT_MISMATCH;
        else if (l->rel.description_bits != pd0_rel_bits(&l->rel)) rc = PD0V_SCORE_BITS_MISMATCH;
        else if (!na || na != nb || memcmp(a, b, na)) rc = PD0V_T8_RELATION_MISMATCH;
    }
    if (!rc && l->confidence_ppm != R->confidence_ppm) rc = PD0V_T8_CONFIDENCE_MISMATCH;
    if (!rc) { if (l->n_exceptions != R->n_exceptions) rc = PD0V_T8_EXCEPTIONS_MISMATCH; else for (uint32_t i = 0; i < R->n_exceptions; i++) if (l->exc[i].record_seq != R->exc[i].record_seq || memcmp(l->exc[i].record_hash, R->exc[i].record_hash, PD0_HASH)) { rc = PD0V_T8_EXCEPTIONS_MISMATCH; break; } }
    if (!rc) for (uint32_t i = 0; i < R->n_experiments; i++) { int found = 0; for (uint32_t j = 0; j < l->n_experiments; j++) if (!memcmp(l->exp[j].experiment_id, R->exp[i].experiment_id, PD0_HASH) && !memcmp(l->exp[j].outcome_hash, R->exp[i].outcome_hash, PD0_HASH) && l->exp[j].result == R->exp[i].result) { found = 1; break; } if (!found) { rc = PD0V_T8_EXPERIMENT_MISSING; break; } }
    if (!rc && memcmp(l->chain_root, R->chain_root, PD0_HASH)) rc = PD0V_T8_CHAIN_ROOT_MISMATCH;
    if (!rc) { uint8_t a[512], b[512]; size_t na = pd0_dom_write(&R->dom, a), nb = pd0_dom_write(&l->dom, b); if (na != nb || memcmp(a, b, na)) rc = PD0V_T8_DOMAIN_MISMATCH; }
    if (!rc) { char want[PD0_MAX_CLAIM]; uint32_t n = pd0_claim_text(R, P, want, sizeof want); if (n != l->claim_len || memcmp(want, l->claim, n)) rc = PD0V_T8_CLAIM_TEXT_MISMATCH; }
    free(l); return rc;
}
