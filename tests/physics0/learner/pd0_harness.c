/* PD-0 development harness for the learner (spec section 11 roles: the harness
 * owns the split, the tags, the ledger and the scoring; the learner only sees
 * FIT/SELECT records). Verifier-side build: links pd0_fmt/pd0_ladder/pd0_score/
 * pd0_controls and the learner library, never the generators. The world and the
 * truth are separate processes (build/physics0/pd0-world, pd0-truth).
 * Usage: pd0-harness <world-bin> <truth-bin> <level|null> <seed> <out-dir>
 * Prints one PD0L result line; writes <out-dir>/pd0l-L<level>-s<seed>.{ledger,law,json}. */
#include "pd0_learner.h"
#include "pd0_ladder.h"
#include "pd0_codes.h"
#include "pd0_score.h"
#include "pd0_controls.h"
#include "pd0_rng.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define EP_LEN 20u
#define LCAP (64u << 20)
#define MAX_EP 1024

typedef struct { pid_t pid; int in, out; FILE *rd, *wr; } proc;
static int spawn(proc *p, char *const argv[])
{
    int a[2], b[2]; if (pipe(a) || pipe(b)) return -1;
    p->pid = fork(); if (p->pid < 0) return -1;
    if (p->pid == 0) { dup2(a[0], 0); dup2(b[1], 1); close(a[0]); close(a[1]); close(b[0]); close(b[1]); execv(argv[0], argv); _exit(127); }
    close(a[0]); close(b[1]); p->out = a[1]; p->in = b[0]; p->rd = fdopen(p->in, "r"); p->wr = fdopen(p->out, "w"); return 0;
}
static int rd_full(int fd, uint8_t *b, size_t n) { while (n) { ssize_t k = read(fd, b, n); if (k <= 0) return -1; b += k; n -= (size_t)k; } return 0; }
static int wr_full(int fd, const uint8_t *b, size_t n) { while (n) { ssize_t k = write(fd, b, n); if (k <= 0) return -1; b += k; n -= (size_t)k; } return 0; }
/* one framed world call; returns response length or 0 */
static size_t world_call(proc *w, const uint8_t *req, size_t n, uint8_t *resp, size_t cap)
{
    uint8_t l4[4]; pd0_put_u32(l4, (uint32_t)n); if (wr_full(w->out, l4, 4) || wr_full(w->out, req, n) || rd_full(w->in, l4, 4)) return 0;
    uint32_t rn = pd0_get_u32(l4); if (rn == 0 || rn > cap || rd_full(w->in, resp, rn)) return 0; return rn;
}
typedef struct { int version; pd0l_desc d; } describe;
static int parse_describe(const uint8_t *b, size_t n, describe *D)
{
    memset(D, 0, sizeof *D); if (n < 10) return -1;
    if (!memcmp(b, "PD0DESC1", 8)) D->version = 1; else if (!memcmp(b, "PD0DESC2", 8)) D->version = 2; else return -1;
    pd0l_desc *d = &D->d; d->n_obs = b[8]; d->n_channels = b[9]; size_t o = 10; if (d->n_obs == 0 || d->n_obs > PD0_MAX_OBS || d->n_channels > PD0_MAX_CHAN) return -1;
    size_t nr = D->version == 2 ? d->n_obs : 1; if (n < o + 8 + 16u * d->n_channels + 16u * nr + 12) return -1;
    d->dt_micro = (int64_t)pd0_get_u64(b + o); o += 8;
    for (int c = 0; c < d->n_channels; c++, o += 8) d->chan_min[c] = (int64_t)pd0_get_u64(b + o);
    for (int c = 0; c < d->n_channels; c++, o += 8) d->chan_max[c] = (int64_t)pd0_get_u64(b + o);
    int64_t mn[PD0_MAX_OBS], mx[PD0_MAX_OBS];
    for (size_t j = 0; j < nr; j++, o += 8) mn[j] = (int64_t)pd0_get_u64(b + o);
    for (size_t j = 0; j < nr; j++, o += 8) mx[j] = (int64_t)pd0_get_u64(b + o);
    for (int j = 0; j < d->n_obs; j++) { d->reset_min[j] = mn[nr == 1 ? 0 : (size_t)j]; d->reset_max[j] = mx[nr == 1 ? 0 : (size_t)j]; }
    d->episode_max_steps = pd0_get_u32(b + o); d->budget_steps = pd0_get_u32(b + o + 4); d->budget_episodes = pd0_get_u32(b + o + 8);
    return 0;
}

/* ---- run state ---- */
typedef struct {
    describe D; int level; uint64_t seed; proc world, truth; pd0_learner *L;
    uint8_t *ledger; size_t llen; uint8_t last[PD0_HASH];
    pd0_ladder_params P;
    uint32_t next_episode, n_episodes, steps_used, episodes_used;
    /* per episode bookkeeping: tag, schedule hash, record range */
    uint8_t ep_tag[MAX_EP]; uint8_t ep_hash[MAX_EP][PD0_HASH]; uint32_t ep_first[MAX_EP], ep_n[MAX_EP]; int ep_ok[MAX_EP];
    pd0_rec *recs; uint32_t nrec;
    pd0_transition *fit; uint32_t nfit;
    /* truth */
    pd0_rel truth_rel; int truth_size, truth_n_latent; int have_truth;
    /* counters for the report */
    uint32_t n_oob, n_refused, n_propose_fail, n_trial_random, n_attempts, n_budget;
} run;

static void append(run *R, uint8_t kind, const uint8_t *p, uint32_t n) { R->llen = pd0_ledg_append(R->ledger, R->llen, LCAP, kind, p, n, R->last); }
static void tag(run *R, uint32_t ep, uint8_t t, uint32_t batch, uint8_t origin) { pd0_tag T = { ep, t, batch, origin }; uint8_t p[32]; append(R, LEDG_TAG, p, (uint32_t)pd0_tag_write(&T, p)); }

/* one episode against the world: TAG first, then reset + steps; records go to the ledger, FIT/SELECT ones to the learner.
 * returns the episode index (bookkeeping slot) or -1 on budget exhaustion */
static int episode(run *R, uint8_t t, uint32_t batch, uint8_t origin, const int64_t *reset, const pd0_step *steps, uint32_t n)
{
    if (R->n_episodes >= MAX_EP) return -1;
    uint32_t ep = R->next_episode; uint32_t slot = R->n_episodes; uint8_t req[128], resp[1024]; pd0_rec r;
    tag(R, ep, t, batch, origin);
    req[0] = 1; req[1] = R->D.d.n_obs; for (int j = 0; j < R->D.d.n_obs; j++) pd0_put_u64(req + 2 + 8 * j, (uint64_t)reset[j]);
    size_t rn = world_call(&R->world, req, 2 + 8u * R->D.d.n_obs, resp, sizeof resp); if (!rn) { fprintf(stderr, "world: reset call failed\n"); exit(3); }
    if (pd0_rec_parse(resp, rn, &r, NULL) != PD0V_OK) { fprintf(stderr, "world: bad reset record\n"); exit(3); }
    if (r.status == PD0_ST_BUDGET_EXHAUSTED) { R->n_budget++; return -1; }
    if (r.episode != ep) { fprintf(stderr, "harness: episode numbering drift (%u vs %u)\n", r.episode, ep); exit(3); }
    R->next_episode = ep + 1; R->episodes_used++;
    R->ep_tag[slot] = t; R->ep_first[slot] = R->nrec; R->ep_n[slot] = 0; R->ep_ok[slot] = 0; R->n_episodes++;
    append(R, LEDG_OBS, resp, (uint32_t)rn); R->recs[R->nrec++] = r; R->ep_n[slot]++;
    if (t == TAG_FIT || t == TAG_SELECT) pd0_learner_observe(R->L, &r, t);
    if (r.status != PD0_ST_OK) { R->n_refused++; memset(R->ep_hash[slot], 0, PD0_HASH); return (int)slot; }
    int64_t reset_seen[PD0_MAX_OBS]; memcpy(reset_seen, r.after, sizeof reset_seen); pd0_step applied[PD0_MAX_STEPS]; uint32_t n_ok = 0;
    for (uint32_t s = 0; s < n; s++) {
        req[0] = 2; req[1] = steps[s].channel; pd0_put_u64(req + 2, (uint64_t)steps[s].value);
        rn = world_call(&R->world, req, 10, resp, sizeof resp); if (!rn) { fprintf(stderr, "world: step call failed\n"); exit(3); }
        if (pd0_rec_parse(resp, rn, &r, NULL) != PD0V_OK) { fprintf(stderr, "world: bad step record\n"); exit(3); }
        append(R, LEDG_OBS, resp, (uint32_t)rn); R->recs[R->nrec++] = r; R->ep_n[slot]++;
        if (t == TAG_FIT || t == TAG_SELECT) pd0_learner_observe(R->L, &r, t);
        if (r.status == PD0_ST_BUDGET_EXHAUSTED) { R->n_budget++; break; }
        R->steps_used++;
        if (r.status == PD0_ST_OK || r.status == PD0_ST_EPISODE_END) { if (n_ok < PD0_MAX_STEPS) { applied[n_ok].channel = r.channel; applied[n_ok].value = r.applied; } n_ok++;
            if (t == TAG_FIT && R->nfit < 65536) { pd0_transition *f = &R->fit[R->nfit++]; memcpy(f->before, r.before, sizeof f->before); f->chan = r.channel; f->value = r.applied; memcpy(f->after, r.after, sizeof f->after); } }
        if (r.status == PD0_ST_OUT_OF_BOUNDS) { R->n_oob++; break; }
        if (r.status == PD0_ST_REFUSED_RANGE) { R->n_refused++; continue; }
        if (r.status == PD0_ST_EPISODE_END) break;
    }
    R->ep_ok[slot] = n_ok >= EP_LEN;
    pd0_schedule_hash(R->D.d.n_obs, reset_seen, (uint8_t)(n_ok < PD0_MAX_STEPS ? n_ok : PD0_MAX_STEPS), applied, R->ep_hash[slot]);
    return (int)slot;
}

/* ---- truth process ---- */
static int truth_rel(run *R)
{
    fprintf(R->truth.wr, "rel\n"); fflush(R->truth.wr); char line[1024]; unsigned nv, nl, nc, ne; int sz, tsz;
    if (!fgets(line, sizeof line, R->truth.rd) || sscanf(line, "rel %u %u %u %u %d %d", &nv, &nl, &nc, &ne, &sz, &tsz) != 6) return -1;
    pd0_rel *t = &R->truth_rel; memset(t, 0, sizeof *t); t->n_vars = (uint8_t)nv; t->n_latent = (uint8_t)nl; t->n_channels = (uint8_t)nc; R->truth_size = tsz; R->truth_n_latent = (int)nl;
    while (fgets(line, sizeof line, R->truth.rd) && strncmp(line, "end", 3)) { unsigned tg; long long coef; int off; if (sscanf(line, "term %u %lld%n", &tg, &coef, &off) != 2) return -1;
        int e; for (e = 0; e < t->n_equations; e++) if (t->eq[e].target == tg) break; if (e == t->n_equations) { t->eq[t->n_equations++].target = (uint8_t)tg; }
        pd0_eq *q = &t->eq[e]; int k = q->n_terms++; q->coef[k] = coef; char *p = line + off; for (unsigned i = 0; i < nv + nc; i++) q->expo[k][i] = (uint8_t)strtoul(p, &p, 10); }
    t->description_bits = pd0_rel_bits(t); R->have_truth = 1; return 0;
}
/* noise-free truth trajectory; returns 1 if it stayed in bounds */
static int truth_episode(run *R, const int64_t *reset, const pd0_step *steps, uint32_t n, int64_t out[][PD0_MAX_OBS])
{
    fprintf(R->truth.wr, "ep %u", R->D.d.n_obs); for (int j = 0; j < R->D.d.n_obs; j++) fprintf(R->truth.wr, " %lld", (long long)reset[j]); fprintf(R->truth.wr, " %u", n);
    for (uint32_t s = 0; s < n; s++) fprintf(R->truth.wr, " %d %lld", steps[s].channel == PD0_CHAN_NONE ? -1 : steps[s].channel, (long long)steps[s].value); fprintf(R->truth.wr, "\n"); fflush(R->truth.wr);
    char line[1024]; if (!fgets(line, sizeof line, R->truth.rd)) return 0; uint32_t got = 0; int ok = 1;
    while (fgets(line, sizeof line, R->truth.rd) && strncmp(line, "end", 3)) { if (!strncmp(line, "oob", 3)) { ok = 0; continue; } char *p = line; for (int j = 0; j < R->D.d.n_obs; j++) out[got][j] = strtoll(p, &p, 10); got++; }
    return ok && got == n;
}

static const char *lvl_name(int level) { static char b[8]; if (level < 0) return "null"; snprintf(b, sizeof b, "L%d", level); return b; }

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: pd0-harness <world> <truth> <level|null> <seed> <out-dir>\n"); return 2; }
    static run R; memset(&R, 0, sizeof R); R.level = strcmp(argv[3], "null") == 0 ? -1 : atoi(argv[3]); R.seed = strtoull(argv[4], NULL, 0);
    char *wargv[] = { argv[1], argv[3], argv[4], NULL }, *targv[] = { argv[2], argv[3], argv[4], NULL };
    if (spawn(&R.world, wargv) || spawn(&R.truth, targv)) { perror("spawn"); return 3; }
    R.ledger = malloc(LCAP); R.recs = malloc(sizeof(pd0_rec) * 65536); R.fit = malloc(sizeof(pd0_transition) * 65536);
    /* describe */
    uint8_t req[128], resp[1024]; req[0] = 0; size_t rn = world_call(&R.world, req, 1, resp, sizeof resp);
    if (!rn || parse_describe(resp, rn, &R.D)) { fprintf(stderr, "world: bad describe\n"); return 3; }
    const pd0l_desc *d = &R.D.d;
    if (truth_rel(&R)) { fprintf(stderr, "truth: bad relation\n"); return 3; }
    /* ladder parameters: spec section 6 bounds for the level; harness knowledge, never the learner's */
    pd0_ladder_params_default(&R.P, d->n_obs, d->n_channels);
    for (int c = 0; c < d->n_channels; c++) { R.P.chan_min[c] = d->chan_min[c]; R.P.chan_max[c] = d->chan_max[c]; }
    R.P.reset_min = d->reset_min[0]; R.P.reset_max = d->reset_max[0]; for (int j = 1; j < d->n_obs; j++) { if (d->reset_min[j] < R.P.reset_min) R.P.reset_min = d->reset_min[j]; if (d->reset_max[j] > R.P.reset_max) R.P.reset_max = d->reset_max[j]; }
    R.P.episode_len = EP_LEN; R.P.dt_micro = d->dt_micro; R.P.size_bound = (uint32_t)(R.truth_size > 0 ? R.truth_size : 4) + 2;
    R.P.eps_bound_micro = R.level == 5 ? 50000 : R.level == 6 ? 30000 : 20000; R.P.noisy = R.level == 5;
    R.L = pd0_learner_new(d, R.seed);
    /* 1. exploration: FIT FIT FIT SELECT HOLDOUT per five (60/20/20), 20-step episodes; HOLDOUT never reaches the learner */
    uint32_t reserve = 5 * EP_LEN + 3 * 10 * EP_LEN + 100; uint32_t n_explore = (d->budget_steps > reserve ? (d->budget_steps - reserve) / EP_LEN : 0);
    if (n_explore + 40 > d->budget_episodes) n_explore = d->budget_episodes > 40 ? d->budget_episodes - 40 : 0;
    int64_t reset[PD0_MAX_OBS]; pd0_step steps[PD0_MAX_STEPS];
    for (uint32_t e = 0; e < n_explore; e++) { uint8_t t = (e % 5) < 3 ? TAG_FIT : (e % 5) == 3 ? TAG_SELECT : TAG_HOLDOUT;
        pd0_learner_explore(R.L, e, reset, steps, EP_LEN); if (episode(&R, t, 0, 0, reset, steps, EP_LEN) < 0) break; }
    /* 2. candidate, falsifier, preregistered trials; on refutation the trial records become FIT and the learner refits */
    pd0_ladder_report rep; memset(&rep, 0, sizeof rep); int checker_rc = pd0_ladder_check(R.ledger, R.llen, &R.P, &rep);
    pd0_corr corr; int have_corr = 0; const pd0l_candidate *best = NULL; uint8_t cand_hash[PD0_HASH] = { 0 }; pd0_rel hyps[PD0_MAX_HYP]; int n_hyp = 0; uint8_t pl[16384];
    static uint8_t forbidden[PD0L_MAX_FORBID][PD0_HASH]; uint32_t n_forb = 0;
    for (uint32_t a = 0; a < 3; a++) {
        R.n_attempts++; pd0_learner_fit(R.L); int nc = pd0_learner_n_candidates(R.L);
        if (!have_corr && pd0_learner_correlation(R.L, R.seed ^ 0xC0DEu, 2000, &corr) == 0) { append(&R, LEDG_CORR, pl, (uint32_t)pd0_corr_write(&corr, pl)); have_corr = 1; }
        best = NULL; for (int i = 0; i < nc; i++) if (!pd0_learner_candidate(R.L, i)->is_null) { best = pd0_learner_candidate(R.L, i); break; }
        if (!best || pd0_learner_candidate(R.L, 0)->is_null) { best = NULL; break; }          /* nothing beats the null: stop honestly */
        n_hyp = 0; hyps[n_hyp++] = best->rel;
        pl[0] = REL_CANDIDATE; size_t n = pd0_rel_write(&hyps[0], pl + 1, sizeof pl - 1); append(&R, LEDG_RELATION, pl, (uint32_t)n + 1); memcpy(cand_hash, R.last, PD0_HASH);
        pd0_falsifier f; memset(&f, 0, sizeof f); memcpy(f.candidate, cand_hash, PD0_HASH); f.eps_micro = R.P.eps_bound_micro; f.min_trials = 5;
        for (int i = 0; i < nc && n_hyp < 4; i++) { const pd0l_candidate *c = pd0_learner_candidate(R.L, i); if (c == best || c->is_null) continue;
            hyps[n_hyp++] = c->rel; pl[0] = REL_RIVAL; n = pd0_rel_write(&c->rel, pl + 1, sizeof pl - 1); append(&R, LEDG_RELATION, pl, (uint32_t)n + 1); memcpy(f.rival[f.n_rivals++], R.last, PD0_HASH); }
        { pd0_rel nullr; memset(&nullr, 0, sizeof nullr); nullr.n_vars = d->n_obs; nullr.n_channels = d->n_channels; hyps[n_hyp++] = nullr;
          pl[0] = REL_NULL; n = pd0_rel_write(&nullr, pl + 1, sizeof pl - 1); append(&R, LEDG_RELATION, pl, (uint32_t)n + 1); memcpy(f.rival[f.n_rivals++], R.last, PD0_HASH); }
        append(&R, LEDG_FALSIFIER, pl, (uint32_t)pd0_fals_write(&f, pl));
        checker_rc = pd0_ladder_check(R.ledger, R.llen, &R.P, &rep); if (rep.state < LS_HYPOTHESIS) break;   /* no admissible candidate: no trials */
        n_forb = 0; for (uint32_t s = 0; s < R.n_episodes && n_forb < PD0L_MAX_FORBID; s++) memcpy(forbidden[n_forb++], R.ep_hash[s], PD0_HASH);
        for (int t = 0; t < 5; t++) { static pd0_exp x; memset(&x, 0, sizeof x);
            int64_t div = pd0_learner_propose(R.L, hyps, n_hyp, R.seed * 1000 + a * 10 + (uint64_t)t, 256, (const uint8_t (*)[PD0_HASH])forbidden, n_forb, &x);
            if (div < 0) { R.n_propose_fail++; R.n_trial_random++; x.n_obs = d->n_obs; x.n_hyp = (uint8_t)n_hyp; x.n_steps = PD0_MAX_STEPS;
                pd0_learner_random_schedule(d, R.seed * 7919 + a, "trial", (uint32_t)t, x.reset, x.steps, PD0_MAX_STEPS);
                for (int h = 0; h < n_hyp; h++) { int64_t tmp[PD0_MAX_STEPS * PD0_MAX_OBS]; pd0_learner_rollout(&hyps[h], x.reset, x.steps, PD0_MAX_STEPS, tmp); for (int s = 0; s < PD0_MAX_STEPS; s++) for (int j = 0; j < d->n_obs; j++) x.expected[h][s][j] = tmp[s * d->n_obs + j]; }
                x.divergence_micro = 0; }
            size_t xn = pd0_exp_write(&x, pl, sizeof pl); append(&R, LEDG_PREREG, pl, (uint32_t)xn);
            int slot = episode(&R, TAG_TRIAL, 0, 0, x.reset, x.steps, PD0_MAX_STEPS); if (slot >= 0 && n_forb < PD0L_MAX_FORBID) memcpy(forbidden[n_forb++], R.ep_hash[slot], PD0_HASH); }
        checker_rc = pd0_ladder_check(R.ledger, R.llen, &R.P, &rep);
        if (rep.state >= LS_INTERVENED) break;
        if (rep.code == PD0V_T6_TRIAL_FAILED) { /* refuted: the trial records are FIT from now on */
            for (uint32_t s = 0; s < R.n_episodes; s++) if (R.ep_tag[s] == TAG_TRIAL) { for (uint32_t i = 0; i < R.ep_n[s]; i++) pd0_learner_observe(R.L, &R.recs[R.ep_first[s] + i], TAG_FIT); R.ep_tag[s] = TAG_FIT; }
            continue; }
        break;
    }
    /* 3. replication: three batches of ten episodes, planner and random halves */
    if (rep.state >= LS_INTERVENED) {
        for (uint32_t bt = 0; bt < 3; bt++) { pd0_batch bb = { bt + 1, R.seed * 100 + bt + 1, 10 }; append(&R, LEDG_BATCH, pl, (uint32_t)pd0_batch_write(&bb, pl));
            for (uint32_t e = 0; e < 10; e++) { uint8_t origin = (e & 1) ? ORIGIN_RANDOM : ORIGIN_PLANNER; static pd0_exp x;
                if (origin == ORIGIN_PLANNER && pd0_learner_propose(R.L, hyps, n_hyp, bb.stream_seed * 31 + e, 128, (const uint8_t (*)[PD0_HASH])forbidden, n_forb, &x) >= 0) { memcpy(reset, x.reset, sizeof reset); memcpy(steps, x.steps, sizeof steps); }
                else { if (origin == ORIGIN_PLANNER) R.n_propose_fail++; pd0_learner_random_schedule(d, bb.stream_seed, "rep", e, reset, steps, PD0_MAX_STEPS); }
                int slot = episode(&R, TAG_REP, bb.batch_id, origin, reset, steps, PD0_MAX_STEPS); if (slot >= 0 && n_forb < PD0L_MAX_FORBID) memcpy(forbidden[n_forb++], R.ep_hash[slot], PD0_HASH); } }
        checker_rc = pd0_ladder_check(R.ledger, R.llen, &R.P, &rep);
    }
    /* 4. law record + independent re-verification */
    static uint8_t law[65536]; size_t law_len = pd0_ladder_emit_law(&rep, &R.P, law, sizeof law); int law_rc = law_len ? pd0_ladder_verify_law(&rep, &R.P, law, law_len) : -1;
    /* 5. scoring against the truth: 10 in-box + 10 extrapolation (1.5x box) episodes from the "score" stream */
    pd0_score_result S; memset(&S, 0, sizeof S); S.code = -1; const pd0_rel *scored = rep.have_candidate ? &rep.candidate : best ? &best->rel : NULL; int n_score_eps = 0;
    if (scored && R.have_truth && R.level >= 0) {
        static pd0_score_episode eps[20]; pd0_rng g; pd0_rng_stream(&g, R.seed + 500, "score"); pd0l_desc box = *d;
        for (int i = 0; i < 20 && n_score_eps < 20; ) { pd0_score_episode *e = &eps[n_score_eps]; memset(e, 0, sizeof *e); e->in_box = i < 10;
            for (int j = 0; j < d->n_obs; j++) { box.reset_min[j] = e->in_box ? d->reset_min[j] : d->reset_min[j] * 3 / 2; box.reset_max[j] = e->in_box ? d->reset_max[j] : d->reset_max[j] * 3 / 2; }
            pd0_learner_random_schedule(&box, R.seed + 500, e->in_box ? "score-in" : "score-ex", (uint32_t)pd0_rng_next(&g), e->init, e->steps, PD0_MAX_STEPS);
            if (truth_episode(&R, e->init, e->steps, PD0_MAX_STEPS, e->truth)) { n_score_eps++; i++; } else if (pd0_rng_next(&g) % 64 == 0) i++; }
        pd0_score_params SP; memset(&SP, 0, sizeof SP); SP.n_obs = d->n_obs; SP.n_channels = d->n_channels; SP.size_bound = R.P.size_bound;
        SP.inbox_bound = R.level == 6 ? 30000 : 20000; SP.extrap_bound = R.level == 6 ? 80000 : 60000; SP.onestep_bound = R.level == 6 ? 0 : 10000;
        SP.score_constants = R.level != 6; SP.const_tol_ppm = R.level == 5 ? 100000 : 50000; SP.require_latent = R.level == 6; SP.latent_ref_factor = 3;
        if (R.level != 6) for (int e = 0; e < R.truth_rel.n_equations; e++) for (int t = 0; t < R.truth_rel.eq[e].n_terms && SP.n_true_terms < PD0_MAX_TRUE_TERMS; t++) { pd0_true_term *tt = &SP.true_terms[SP.n_true_terms++]; tt->target = R.truth_rel.eq[e].target; memcpy(tt->expo, R.truth_rel.eq[e].expo[t], sizeof tt->expo); tt->coef = R.truth_rel.eq[e].coef[t]; }
        pd0_rel rc = *scored; pd0_score(&SP, &rc, 0, pd0_rel_rollout_fn, pd0_rel_predict_fn, &rc, eps, (uint32_t)n_score_eps, R.fit, R.nfit, &S);
    }
    /* 6. outputs */
    char path[1024]; snprintf(path, sizeof path, "%s/pd0l-%s-s%llu.ledger", argv[5], lvl_name(R.level), (unsigned long long)R.seed); FILE *fo = fopen(path, "wb"); if (fo) { fwrite(R.ledger, 1, R.llen, fo); fclose(fo); }
    if (law_len) { snprintf(path, sizeof path, "%s/pd0l-%s-s%llu.law", argv[5], lvl_name(R.level), (unsigned long long)R.seed); fo = fopen(path, "wb"); if (fo) { fwrite(law, 1, law_len, fo); fclose(fo); } }
    static char jb[1 << 20]; pd0_learner_report(R.L, jb, sizeof jb); snprintf(path, sizeof path, "%s/pd0l-%s-s%llu.json", argv[5], lvl_name(R.level), (unsigned long long)R.seed); fo = fopen(path, "w");
    if (fo) { fprintf(fo, "{\"level\":\"%s\",\"seed\":%llu,\"describe_version\":%d,\"state\":\"%s\",\"code\":%d,\"refutations\":%u,\"score_code\":%d,\"score_mask\":%u,\"learner\":%s}\n", lvl_name(R.level), (unsigned long long)R.seed, R.D.version, pd0_ladder_state_name(rep.state), rep.code, rep.n_refutations, S.code, S.fail_mask, jb); fclose(fo); }
    uint32_t cbits = scored ? pd0_rel_bits(scored) : 0, csize = scored ? pd0_rel_size(scored) : 0; int hidden = best ? best->hidden_state_suspected : 0;
    printf("PD0L level=%s seed=%llu desc=v%d episodes=%u steps=%u fit=%u select=%u state=%s code=%d stall=%d refutations=%u attempts=%u "
           "cand_size=%u cand_bits=%u size_bound=%u fit_nrmse=%lld select_nrmse=%lld hidden_state=%d "
           "score=%s score_code=%d mask=0x%x inbox=%lld extrap=%lld onestep=%lld ref=%lld score_eps=%d law=%d checker_rc=%d "
           "oob=%u refused=%u budget_hits=%u propose_fail=%u trial_random=%u\n",
           lvl_name(R.level), (unsigned long long)R.seed, R.D.version, R.n_episodes, R.steps_used, pd0_learner_n_transitions(R.L, TAG_FIT), pd0_learner_n_transitions(R.L, TAG_SELECT),
           pd0_ladder_state_name(rep.state), rep.code, rep.stall_code, rep.n_refutations, R.n_attempts,
           csize, cbits, R.P.size_bound, (long long)(best ? best->fit_nrmse_micro : -1), (long long)(best ? best->select_nrmse_micro : -1), hidden,
           !scored ? "NO_CANDIDATE" : S.code == 0 ? "PASS" : S.code < 0 ? "NOT_SCORED" : "FAIL", S.code, S.fail_mask, (long long)S.inbox_nrmse, (long long)S.extrap_nrmse, (long long)S.onestep_nrmse, (long long)S.ref_nrmse, n_score_eps, law_rc, checker_rc,
           R.n_oob, R.n_refused, R.n_budget, R.n_propose_fail, R.n_trial_random);
    fprintf(R.truth.wr, "quit\n"); fflush(R.truth.wr); fclose(R.world.wr); fclose(R.truth.wr); waitpid(R.world.pid, NULL, 0); waitpid(R.truth.pid, NULL, 0);
    pd0_learner_free(R.L); free(R.ledger); free(R.recs); free(R.fit);
    return 0;
}
