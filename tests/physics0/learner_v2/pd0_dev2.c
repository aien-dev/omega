/* PD-0 learner v2 development driver (adaptive-intelligence milestone 3, gap G3). DEVELOPMENT EVIDENCE ONLY.
 * Harness side: owns the split, builds an in-process world through pd0_dev_world (bytes in, bytes out), and forwards
 * only FIT and SELECT episodes to the learner. TEST episodes are scored here and never reach the learner. No PD-0b
 * holdout, trial or replication stream is used; seeds are public dev seeds given on the command line.
 *
 *   pd0-dev2 flag   <seed_lo> <seed_hi>   C1: hidden-state flag + prediction quality per level and seed
 *   pd0-dev2 regime <seed_lo> <seed_hi>   C2: regime switch, FIXED vs UPDATING (retain) vs FRESH, per level and seed
 *   pd0-dev2 prior  <seed_lo> <seed_hi>   C3: wrong / stale prior injected into the updating learner
 *   pd0-dev2 detect <seed_lo> <seed_hi>   v2.1 C1: change-detector decision on no-switch and switch runs
 *   pd0-dev2 shift  <seed_lo> <seed_hi>   v2.1 C2: three arms after a regime switch + no-switch false-alarm run
 *   pd0-dev2 prior21 <seed_lo> <seed_hi>  v2.1 C3: wrong / stale prior, v2 arm then v2.1 arm
 * Built twice: with -DDEV_V1 against the seal-1 learner (flag mode only, the baseline) and against learner_v2. */
#include "pd0_learner.h"
#include "pd0_dev_world.h"
#include "pd0_codes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEV_V1
#define LNAME "v1"
#else
#define LNAME "v2"
#endif
#define EP_LEN 20u
#define MAXEP 64
#define NLEV 8
#define NK 4
__attribute__((unused)) static const uint32_t KS[NK] = { 4, 8, 16, 32 };      /* regime-2 training episodes seen at each checkpoint (3 FIT : 1 SELECT) */

typedef struct { uint32_t n; pd0_rec r[1 + EP_LEN]; } ep_t;
typedef struct { dev_world *w; pd0l_desc d; uint64_t seed; int level; } sess;

static const char *lname(int l) { static char b[NLEV][8]; if (l == 7) return "null"; snprintf(b[l], 8, "L%d", l); return b[l]; }
static size_t call(sess *S, const uint8_t *q, size_t n, uint8_t *resp, size_t cap) { size_t r = dev_world_call(S->w, q, n, resp, cap); if (!r) { fprintf(stderr, "world call failed\n"); exit(3); } return r; }

static int parse_describe(const uint8_t *b, size_t n, pd0l_desc *d)
{
    memset(d, 0, sizeof *d); if (n < 10 || memcmp(b, "PD0DESC2", 8)) return -1;
    d->n_obs = b[8]; d->n_channels = b[9]; size_t o = 10, nr = d->n_obs;
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
static int open_sess(sess *S, int level, uint64_t seed)
{
    memset(S, 0, sizeof *S); S->level = level; S->seed = seed; S->w = dev_world_open(level, seed); if (!S->w) return -1;
    uint8_t q[1] = { 0 }, resp[1024]; size_t n = call(S, q, 1, resp, sizeof resp); return parse_describe(resp, n, &S->d);
}
/* one episode against the world: a seeded schedule from stream `tag`/idx inside the describe bounds */
static void run_ep(sess *S, ep_t *e, const char *tag, uint32_t idx)
{
    int64_t reset[PD0_MAX_OBS]; pd0_step st[EP_LEN]; uint8_t q[128], resp[1024]; memset(e, 0, sizeof *e);
    pd0_learner_random_schedule(&S->d, S->seed, tag, idx, reset, st, EP_LEN);
    q[0] = 1; q[1] = S->d.n_obs; for (int j = 0; j < S->d.n_obs; j++) pd0_put_u64(q + 2 + 8 * j, (uint64_t)reset[j]);
    size_t n = call(S, q, 2 + 8u * S->d.n_obs, resp, sizeof resp);
    if (pd0_rec_parse(resp, n, &e->r[0], NULL) != PD0V_OK) { fprintf(stderr, "bad reset record\n"); exit(3); }
    e->n = 1; if (e->r[0].status != PD0_ST_OK) return;
    for (uint32_t s = 0; s < EP_LEN; s++) {
        q[0] = 2; q[1] = st[s].channel; pd0_put_u64(q + 2, (uint64_t)st[s].value); n = call(S, q, 10, resp, sizeof resp);
        if (pd0_rec_parse(resp, n, &e->r[e->n], NULL) != PD0V_OK) { fprintf(stderr, "bad step record\n"); exit(3); }
        uint8_t stt = e->r[e->n++].status; if (stt == PD0_ST_EPISODE_END || stt == PD0_ST_OUT_OF_BOUNDS || stt == PD0_ST_BUDGET_EXHAUSTED) break; }
}
static void feed(pd0_learner *L, const ep_t *e, uint8_t tag) { for (uint32_t i = 0; i < e->n; i++) pd0_learner_observe(L, &e->r[i], tag); }

/* ---- prediction error of a relation on TEST episodes (same normalisation as the learner: worst variable, rmse / sd of truth) ---- */
typedef struct { double se[PD0_MAX_OBS], s[PD0_MAX_OBS], ss[PD0_MAX_OBS]; uint32_t n; } macc;
static void madd(macc *a, const int64_t *p, const int64_t *t, int no) { for (int j = 0; j < no; j++) { double e = (double)(p[j] - t[j]) / 1e6, v = (double)t[j] / 1e6; a->se[j] += e * e; a->s[j] += v; a->ss[j] += v * v; } a->n++; }
static int64_t mnrmse(const macc *a, int no)
{
    double w = 0; if (!a->n) return 0;
    for (int j = 0; j < no; j++) { double rmse = sqrt(a->se[j] / a->n), m = a->s[j] / a->n, var = a->ss[j] / a->n - m * m, sd = var > 0 ? sqrt(var) : 0, v = sd > 1e-9 ? rmse / sd : rmse; if (v > w) w = v; }
    return w > 9e12 ? INT64_MAX : (int64_t)(w * 1e6);
}
static void eval_rel(const pd0_rel *rel, const ep_t *te, int nte, int no, int64_t *roll, int64_t *one)
{
    macc ar, ao; memset(&ar, 0, sizeof ar); memset(&ao, 0, sizeof ao);
    for (int i = 0; i < nte; i++) { const ep_t *e = &te[i]; pd0_step st[EP_LEN]; int64_t truth[EP_LEN][PD0_MAX_OBS], before[EP_LEN][PD0_MAX_OBS], out[EP_LEN * PD0_MAX_OBS]; uint32_t ns = 0;
        for (uint32_t k = 1; k < e->n; k++) { if (e->r[k].status != PD0_ST_OK && e->r[k].status != PD0_ST_EPISODE_END) continue;
            st[ns].channel = e->r[k].channel; st[ns].value = e->r[k].applied; memcpy(truth[ns], e->r[k].after, sizeof truth[ns]); memcpy(before[ns], e->r[k].before, sizeof before[ns]); ns++; }
        if (!ns) continue;
        pd0_learner_rollout(rel, e->r[0].after, st, ns, out);
        for (uint32_t k = 0; k < ns; k++) { madd(&ar, &out[k * no], truth[k], no);
            int64_t s0[PD0_MAX_VARS], nx[PD0_MAX_VARS]; memset(s0, 0, sizeof s0); for (int j = 0; j < no; j++) s0[j] = before[k][j];
            pd0_rel_step(rel, s0, st[k].channel, st[k].value, nx); madd(&ao, nx, truth[k], no); } }
    *roll = mnrmse(&ar, no); *one = mnrmse(&ao, no);
}

/* ---- regime-1 protocol: the harness split, FIT FIT FIT SELECT TEST per five, 20-step episodes ---- */
typedef struct { ep_t train[MAXEP * 4]; uint8_t tag[MAXEP * 4]; int n_train; ep_t test[MAXEP]; int n_test; } r1data;
static r1data *collect_r1(sess *S)
{
    static r1data D; memset(&D, 0, sizeof D);
    uint32_t reserve = 5 * EP_LEN + 3 * 10 * EP_LEN + 100, n_explore = S->d.budget_steps > reserve ? (S->d.budget_steps - reserve) / EP_LEN : 0;
    if (n_explore + 40 > S->d.budget_episodes) n_explore = S->d.budget_episodes > 40 ? S->d.budget_episodes - 40 : 0;
    ep_t *tmp = malloc(sizeof *tmp);
    for (uint32_t e = 0; e < n_explore; e++) { int m = (int)(e % 5);
        if (m == 4) { if (D.n_test < MAXEP) { run_ep(S, &D.test[D.n_test], "test1", (uint32_t)D.n_test); D.n_test++; } continue; }
        if ((size_t)D.n_train >= sizeof D.train / sizeof D.train[0]) break;
        run_ep(S, tmp, "explore", e); D.train[D.n_train] = *tmp; D.tag[D.n_train] = m == 3 ? TAG_SELECT : TAG_FIT; D.n_train++; }
    free(tmp); return &D;
}
static int g_forget;   /* v2.1 switch for build(): 1 = detector + bounded forgetting on */
static pd0_learner *build(sess *S, const r1data *D, const ep_t *r2, int k2, int with_r1, const pd0_rel *prior)
{
    pd0_learner *L = pd0_learner_new(&S->d, S->seed);
#ifndef DEV_V1
    pd0_learner_set_forgetting(L, g_forget);
#endif
    if (with_r1) for (int i = 0; i < D->n_train; i++) feed(L, &D->train[i], D->tag[i]);
    for (int i = 0; i < k2; i++) feed(L, &r2[i], i % 4 == 3 ? TAG_SELECT : TAG_FIT);
#ifndef DEV_V1
    if (prior) pd0_learner_set_prior(L, prior);
#else
    (void)prior;
#endif
    pd0_learner_fit(L); return L;
}
static const pd0l_candidate *best_cand(const pd0_learner *L) { return pd0_learner_candidate(L, 0); }

static int cmpi(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static int64_t median(int64_t *v, int n) { if (!n) return 0; qsort(v, (size_t)n, sizeof *v, cmpi); return v[n / 2]; }
__attribute__((unused)) static int tie(int64_t a, int64_t b) { int64_t m = a < b ? a : b, d = a > b ? a - b : b - a, tol = m / 5 > 200 ? m / 5 : 200; return d <= tol; }
__attribute__((unused)) static const char *verdict(int64_t a, int64_t b) { return tie(a, b) ? "TIE" : a < b ? "HELPS" : "HARMS"; }   /* a = the arm with retained knowledge */

/* ================= C1 ================= */
static int mode_flag(uint64_t lo, uint64_t hi)
{
    for (int level = 0; level < NLEV; level++) { int fired = 0, nseed = 0; int64_t rolls[64], ones[64]; int nr = 0;
        for (uint64_t seed = lo; seed <= hi; seed++) { sess S; if (open_sess(&S, level, seed)) { fprintf(stderr, "open failed %d %llu\n", level, (unsigned long long)seed); return 3; }
            r1data *D = collect_r1(&S); pd0_learner *L = build(&S, D, NULL, 0, 1, NULL); const pd0l_candidate *c0 = best_cand(L), *best = NULL;
            for (int i = 0; i < pd0_learner_n_candidates(L); i++) if (!pd0_learner_candidate(L, i)->is_null) { best = pd0_learner_candidate(L, i); break; }
            if (c0->is_null) best = NULL;                                          /* nothing beats the null: stop honestly, as the harness does */
            int64_t roll = 0, one = 0; eval_rel(c0 ? &c0->rel : NULL, D->test, D->n_test, S.d.n_obs, &roll, &one);
            int hid = best ? best->hidden_state_suspected : 0; fired += hid; nseed++; if (nr < 64) { rolls[nr] = roll; ones[nr] = one; nr++; }
#ifdef DEV_V1
            printf("P0V2F learner=%s level=%s seed=%llu cand0=%s size=%u sel1=%lld selroll=- by_roll=- test_roll20=%lld test_one=%lld hidden=%d\n", LNAME, lname(level), (unsigned long long)seed,
                   c0->is_null ? "null" : "fit", c0->size, (long long)c0->select_nrmse_micro, (long long)roll, (long long)one, hid);
#else
            printf("P0V2F learner=%s level=%s seed=%llu cand0=%s size=%u sel1=%lld selroll=%lld by_roll=%d test_roll20=%lld test_one=%lld hidden=%d\n", LNAME, lname(level), (unsigned long long)seed,
                   c0->is_null ? "null" : "fit", c0->size, (long long)c0->select_nrmse_micro, (long long)(best ? best->select_roll_nrmse_micro : 0), best ? best->hidden_by_rollout : 0, (long long)roll, (long long)one, hid);
#endif
            pd0_learner_free(L); dev_world_close(S.w); }
        printf("P0V2F-SUM learner=%s level=%s seeds=%d hidden_fired=%d median_test_roll20=%lld median_test_one=%lld\n", LNAME, lname(level), nseed, fired, (long long)median(rolls, nr), (long long)median(ones, nr)); }
    return 0;
}

/* ================= C2 ================= */
#ifndef DEV_V1
typedef struct { int64_t fixed[NK][64], retain[NK][64], fresh[NK][64]; int n; } c2sum;
static int mode_regime(uint64_t lo, uint64_t hi)
{
    for (int level = 0; level < NLEV; level++) { static c2sum Z; memset(&Z, 0, sizeof Z); int applicable = 0;
        for (uint64_t seed = lo; seed <= hi && Z.n < 64; seed++) { sess S; if (open_sess(&S, level, seed)) return 3;
            r1data *D = collect_r1(&S); int no = S.d.n_obs; pd0_learner *L1 = build(&S, D, NULL, 0, 1, NULL); pd0_rel F = best_cand(L1)->rel; int f_null = best_cand(L1)->is_null; pd0_learner_free(L1);
            int64_t pre_roll, pre_one; eval_rel(&F, D->test, D->n_test, no, &pre_roll, &pre_one);
            int64_t ppm = 0; if (dev_world_switch(S.w, 1, &ppm)) { fprintf(stderr, "no valid regime draw\n"); return 3; } if (ppm > 0) applicable = 1;
            static ep_t te[20], tr[32]; for (int i = 0; i < 20; i++) run_ep(&S, &te[i], "test2", (uint32_t)i); for (int i = 0; i < 32; i++) run_ep(&S, &tr[i], "r2", (uint32_t)i);
            int64_t fx_roll, fx_one; eval_rel(&F, te, 20, no, &fx_roll, &fx_one);
            for (int ki = 0; ki < NK; ki++) { pd0_learner *Lr = build(&S, D, tr, (int)KS[ki], 1, NULL), *Lf = build(&S, D, tr, (int)KS[ki], 0, NULL);
                int64_t rr, ro, fr, fo; eval_rel(&best_cand(Lr)->rel, te, 20, no, &rr, &ro); eval_rel(&best_cand(Lf)->rel, te, 20, no, &fr, &fo);
                printf("P0V2R level=%s seed=%llu const_change_ppm=%lld k=%u pre_switch_roll20=%lld fixed_roll20=%lld retain_roll20=%lld fresh_roll20=%lld retain_vs_fresh=%s retain_vs_fixed=%s "
                       "fixed_one=%lld retain_one=%lld fresh_one=%lld fixed_null=%d retain_null=%d fresh_null=%d\n", lname(level), (unsigned long long)seed, (long long)ppm, KS[ki], (long long)pre_roll,
                       (long long)fx_roll, (long long)rr, (long long)fr, verdict(rr, fr), verdict(rr, fx_roll), (long long)fx_one, (long long)ro, (long long)fo, f_null, best_cand(Lr)->is_null, best_cand(Lf)->is_null);
                Z.fixed[ki][Z.n] = fx_roll; Z.retain[ki][Z.n] = rr; Z.fresh[ki][Z.n] = fr; pd0_learner_free(Lr); pd0_learner_free(Lf); }
            Z.n++; dev_world_close(S.w); }
        for (int ki = 0; ki < NK; ki++) { int h = 0, t = 0, x = 0, uh = 0, ut = 0, ux = 0;
            for (int i = 0; i < Z.n; i++) { const char *v = verdict(Z.retain[ki][i], Z.fresh[ki][i]); if (v[1] == 'E') h++; else if (v[0] == 'T') t++; else x++;
                const char *u = verdict(Z.retain[ki][i], Z.fixed[ki][i]); if (u[1] == 'E') uh++; else if (u[0] == 'T') ut++; else ux++; }
            int64_t a[64], b[64], c[64]; memcpy(a, Z.fixed[ki], sizeof a); memcpy(b, Z.retain[ki], sizeof b); memcpy(c, Z.fresh[ki], sizeof c);
            printf("P0V2R-SUM level=%s applicable=%d k=%u seeds=%d median_fixed=%lld median_retain=%lld median_fresh=%lld retain_vs_fresh(helps/tie/harms)=%d/%d/%d retain_vs_fixed(helps/tie/harms)=%d/%d/%d\n",
                   lname(level), applicable, KS[ki], Z.n, (long long)median(a, Z.n), (long long)median(b, Z.n), (long long)median(c, Z.n), h, t, x, uh, ut, ux); } }
    return 0;
}


/* ================= C1 of v2.1: change detector decisions =================
 * pd0-dev2 detect <lo> <hi>: per level and seed, one NO-SWITCH run (32 more episodes from the same world) and one SWITCH run
 * (regime 1 then 32 episodes of the redrawn world); the learner (forgetting on) is fed every episode and the detector decision
 * is printed. delay = post-switch episodes consumed when the alarm fired (1 = the first one). */
static pd0_learner *build21(sess *S, const r1data *D, const ep_t *r2, int k2, int forget)
{
    pd0_learner *L = pd0_learner_new(&S->d, S->seed); pd0_learner_set_forgetting(L, forget);
    for (int i = 0; i < D->n_train; i++) feed(L, &D->train[i], D->tag[i]);
    for (int i = 0; i < k2; i++) feed(L, &r2[i], i % 4 == 3 ? TAG_SELECT : TAG_FIT);
    pd0_learner_fit(L); return L;
}
static int mode_detect(uint64_t lo, uint64_t hi)
{
    for (int level = 0; level < NLEV; level++) { int fa = 0, runs = 0, det = 0, nd = 0; int64_t delays[64];
        for (uint64_t seed = lo; seed <= hi; seed++) for (int sw = 0; sw < 2; sw++) { sess S; if (open_sess(&S, level, seed)) return 3;
            r1data *D = collect_r1(&S); int64_t ppm = 0; if (sw && dev_world_switch(S.w, 1, &ppm)) return 3;
            static ep_t tr[32]; for (int i = 0; i < 32; i++) run_ep(&S, &tr[i], "r2", (uint32_t)i);
            pd0_learner *L = build21(&S, D, tr, 32, 1); const pd0l_detect *d = pd0_learner_detection(L);
            long delay = d->alarm ? (long)d->alarm_ep - D->n_train + 1 : 0;
            printf("P0V21D level=%s seed=%llu run=%s const_change_ppm=%lld episodes=%u base_ep=%u alarm=%d alarm_ep=%u change_ep=%u switch_ep=%d delay=%ld cusum_max=%.2f old_kept=%d\n", lname(level), (unsigned long long)seed,
                   sw ? "switch" : "noswitch", (long long)ppm, d->n_ep, d->base_ep, d->alarm, d->alarm_ep, d->change_ep, sw ? D->n_train : -1, delay, d->cusum_max, d->old_kept);
            if (!sw) { fa += d->alarm; runs++; } else if (ppm > 0) { nd++; if (d->alarm) { if (det < 64) delays[det] = delay; det++; } }
            pd0_learner_free(L); dev_world_close(S.w); }
        printf("P0V21D-SUM level=%s false_alarms=%d/%d switch_detected=%d/%d median_delay_episodes=%lld\n", lname(level), fa, runs, det, nd, (long long)median(delays, det < 64 ? det : 64)); }
    return 0;
}

/* ================= C2 of v2.1: three arms after a regime switch =================
 * pd0-dev2 shift <lo> <hi>: per level and seed, the C2 protocol of #397 (regime 1 data, switch, 20 TEST episodes of the new regime,
 * k regime-2 training episodes) with arms FIXED (regime-1 model, no updating), RETAIN (v2: updating, keeps every old episode),
 * V21 (v2.1: updating + detector + bounded forgetting), and FRESH (regime-2 episodes only; the reference from #397).
 * Then one NO-SWITCH run per seed (32 more episodes of the same world) for the false-alarm rate. Error = median 20-step rollout NRMSE (micro) on the TEST episodes. */
static int mode_shift(uint64_t lo, uint64_t hi)
{
    static const int KI[2] = { 0, 3 };            /* k = 4 and k = 32 are the headline cells; 8 and 16 are printed per seed as well */
    for (int level = 0; level < NLEV; level++) { static int64_t fx[NK][64], rt[NK][64], fr[NK][64], v21[NK][64]; int n = 0, applicable = 0, fa = 0, nfa = 0, det = 0; int64_t delays[64]; (void)KI;
        for (uint64_t seed = lo; seed <= hi && n < 64; seed++) { sess S; if (open_sess(&S, level, seed)) return 3;
            r1data *D = collect_r1(&S); int no = S.d.n_obs; g_forget = 0; pd0_learner *L1 = build(&S, D, NULL, 0, 1, NULL); pd0_rel F = best_cand(L1)->rel; pd0_learner_free(L1);
            int64_t ppm = 0; if (dev_world_switch(S.w, 1, &ppm)) return 3; if (ppm > 0) applicable = 1;
            static ep_t te[20], tr[32]; for (int i = 0; i < 20; i++) run_ep(&S, &te[i], "test2", (uint32_t)i); for (int i = 0; i < 32; i++) run_ep(&S, &tr[i], "r2", (uint32_t)i);
            int64_t fx_roll, fx_one; eval_rel(&F, te, 20, no, &fx_roll, &fx_one);
            for (int ki = 0; ki < NK; ki++) { int64_t rr, fo, hr, o1;
                g_forget = 0; pd0_learner *Lr = build(&S, D, tr, (int)KS[ki], 1, NULL), *Lf = build(&S, D, tr, (int)KS[ki], 0, NULL); g_forget = 1; pd0_learner *Lv = build(&S, D, tr, (int)KS[ki], 1, NULL); g_forget = 0;
                int64_t frr; eval_rel(&best_cand(Lr)->rel, te, 20, no, &rr, &o1); eval_rel(&best_cand(Lf)->rel, te, 20, no, &frr, &fo); eval_rel(&best_cand(Lv)->rel, te, 20, no, &hr, &o1);
                const pd0l_detect *d = pd0_learner_detection(Lv); long delay = d->alarm ? (long)d->alarm_ep - D->n_train + 1 : 0;
                printf("P0V21R level=%s seed=%llu const_change_ppm=%lld k=%u fixed_roll20=%lld retain_roll20=%lld v21_roll20=%lld fresh_roll20=%lld v21_vs_fresh=%s v21_vs_retain=%s alarm=%d alarm_ep=%u change_ep=%u switch_ep=%d delay=%ld v21_top_is_old=%d\n",
                       lname(level), (unsigned long long)seed, (long long)ppm, KS[ki], (long long)fx_roll, (long long)rr, (long long)hr, (long long)frr, verdict(hr, frr), verdict(hr, rr), d->alarm, d->alarm_ep, d->change_ep, D->n_train, delay, best_cand(Lv)->is_old);
                fx[ki][n] = fx_roll; rt[ki][n] = rr; fr[ki][n] = frr; v21[ki][n] = hr;
                if (ki == NK - 1 && ppm > 0 && d->alarm) { if (det < 64) delays[det] = delay; det++; }
                pd0_learner_free(Lr); pd0_learner_free(Lf); pd0_learner_free(Lv); }
            n++; dev_world_close(S.w);
            { sess S2; if (open_sess(&S2, level, seed)) return 3; r1data *D2 = collect_r1(&S2); static ep_t t2[32]; for (int i = 0; i < 32; i++) run_ep(&S2, &t2[i], "r2", (uint32_t)i);
              g_forget = 1; pd0_learner *Ln = build(&S2, D2, t2, 32, 1, NULL); g_forget = 0; const pd0l_detect *d = pd0_learner_detection(Ln); fa += d->alarm; nfa++;
              printf("P0V21N level=%s seed=%llu run=noswitch alarm=%d alarm_ep=%u change_ep=%u cusum_max=%.2f\n", lname(level), (unsigned long long)seed, d->alarm, d->alarm_ep, d->change_ep, d->cusum_max); pd0_learner_free(Ln); dev_world_close(S2.w); } }
        for (int ki = 0; ki < NK; ki++) { int h = 0, t = 0, x = 0, rh = 0, rt2 = 0, rx = 0; int64_t a[64], b[64], c[64], e[64];
            for (int i = 0; i < n; i++) { const char *v = verdict(v21[ki][i], fr[ki][i]); if (v[1] == 'E') h++; else if (v[0] == 'T') t++; else x++;
                const char *u = verdict(v21[ki][i], rt[ki][i]); if (u[1] == 'E') rh++; else if (u[0] == 'T') rt2++; else rx++; }
            memcpy(a, fx[ki], sizeof a); memcpy(b, rt[ki], sizeof b); memcpy(c, fr[ki], sizeof c); memcpy(e, v21[ki], sizeof e);
            printf("P0V21R-SUM level=%s applicable=%d k=%u seeds=%d median_fixed=%lld median_retain=%lld median_v21=%lld median_fresh=%lld v21_vs_fresh(helps/tie/harms)=%d/%d/%d v21_vs_retain(helps/tie/harms)=%d/%d/%d\n",
                   lname(level), applicable, KS[ki], n, (long long)median(a, n), (long long)median(b, n), (long long)median(e, n), (long long)median(c, n), h, t, x, rh, rt2, rx); }
        printf("P0V21-DETECT level=%s false_alarms_noswitch=%d/%d switch_detected_by_k32=%d/%d median_delay_episodes=%lld\n", lname(level), fa, nfa, det, applicable ? n : 0, (long long)median(delays, det < 64 ? det : 64)); }
    return 0;
}
/* ================= C3 ================= */
static int same_support(const pd0_rel *a, const pd0_rel *b)
{
    int na = 0, nb = 0; for (int e = 0; e < a->n_equations; e++) na += a->eq[e].n_terms; for (int e = 0; e < b->n_equations; e++) nb += b->eq[e].n_terms; if (na != nb) return 0;
    for (int e = 0; e < a->n_equations; e++) for (int t = 0; t < a->eq[e].n_terms; t++) { int found = 0;
        for (int f = 0; f < b->n_equations && !found; f++) if (b->eq[f].target == a->eq[e].target) for (int u = 0; u < b->eq[f].n_terms; u++)
            if (!memcmp(a->eq[e].expo[t], b->eq[f].expo[u], sizeof a->eq[e].expo[t])) { found = 1; break; }
        if (!found) return 0; }
    return 1;
}
static pd0_rel wrong_of(const pd0_rel *r)   /* the regime-1 lesson with the sign of its largest coefficient flipped */
{
    pd0_rel w = *r; int be = -1, bt = -1; int64_t bm = -1;
    for (int e = 0; e < w.n_equations; e++) for (int t = 0; t < w.eq[e].n_terms; t++) { int64_t m = llabs(w.eq[e].coef[t]); if (m > bm) { bm = m; be = e; bt = t; } }
    if (be >= 0) w.eq[be].coef[bt] = -w.eq[be].coef[bt];
    return w;
}
static const char *outcome(const pd0_learner *L, const pd0_rel *prior, int *prank)
{
    *prank = -1; for (int i = 0; i < pd0_learner_n_candidates(L); i++) if (pd0_learner_candidate(L, i)->is_prior) { *prank = i; break; }
    const pd0l_candidate *c0 = pd0_learner_candidate(L, 0);
    if (c0->is_prior) return "KEPT"; if (c0->is_null) return "REJECTED_TO_NULL"; return same_support(&c0->rel, prior) ? "CORRECTED" : "REJECTED_OTHER_TERMS";
}
static int mode_prior(uint64_t lo, uint64_t hi)
{
    static const char *PN[2] = { "wrong", "stale" };
    for (int level = 0; level < NLEV; level++) { static int cnt[2][NK + 1][4]; static int64_t vb[2][NK + 1][64], vp[2][NK + 1][64], vn[2][NK + 1][64]; static int nv[2][NK + 1]; memset(cnt, 0, sizeof cnt); memset(nv, 0, sizeof nv); int nskip = 0, nseed = 0;
        for (uint64_t seed = lo; seed <= hi; seed++) { sess S; if (open_sess(&S, level, seed)) return 3;
            r1data *D = collect_r1(&S); int no = S.d.n_obs; pd0_learner *L1 = build(&S, D, NULL, 0, 1, NULL);
            if (best_cand(L1)->is_null) { printf("P0V2P level=%s seed=%llu SKIP regime-1 learner found nothing better than the null, no lesson to corrupt\n", lname(level), (unsigned long long)seed); nskip++; pd0_learner_free(L1); dev_world_close(S.w); continue; }
            nseed++; pd0_rel pri[2]; pri[0] = wrong_of(&best_cand(L1)->rel); pri[1] = best_cand(L1)->rel; pd0_learner_free(L1);
            /* negative control, before any switch: can the ranking reject a wrong lesson at all? */
            { pd0_learner *Lp = build(&S, D, NULL, 0, 1, &pri[0]); int pr; const char *o = outcome(Lp, &pri[0], &pr); int64_t br, bo, wr, wo;
              eval_rel(&best_cand(Lp)->rel, D->test, D->n_test, no, &br, &bo); eval_rel(&pri[0], D->test, D->n_test, no, &wr, &wo);
              printf("P0V2P level=%s seed=%llu prior=wrong k=pre outcome=%s prior_rank=%d best_roll20=%lld prior_roll20=%lld\n", lname(level), (unsigned long long)seed, o, pr, (long long)br, (long long)wr);
              cnt[0][NK][!strcmp(o, "KEPT") ? 0 : !strcmp(o, "CORRECTED") ? 1 : !strcmp(o, "REJECTED_OTHER_TERMS") ? 2 : 3]++; vb[0][NK][nv[0][NK]] = br; vp[0][NK][nv[0][NK]] = wr; vn[0][NK][nv[0][NK]] = br; nv[0][NK]++; pd0_learner_free(Lp); }
            int64_t ppm; dev_world_switch(S.w, 1, &ppm);
            static ep_t te[20], tr[32]; for (int i = 0; i < 20; i++) run_ep(&S, &te[i], "test2", (uint32_t)i); for (int i = 0; i < 32; i++) run_ep(&S, &tr[i], "r2", (uint32_t)i);
            for (int ki = 0; ki < NK; ki++) { pd0_learner *Ln = build(&S, D, tr, (int)KS[ki], 1, NULL); int64_t nr, no1; eval_rel(&best_cand(Ln)->rel, te, 20, no, &nr, &no1); pd0_learner_free(Ln);
                for (int p = 0; p < 2; p++) { pd0_learner *Lp = build(&S, D, tr, (int)KS[ki], 1, &pri[p]); int pr; const char *o = outcome(Lp, &pri[p], &pr); int64_t br, bo, wr, wo;
                    eval_rel(&best_cand(Lp)->rel, te, 20, no, &br, &bo); eval_rel(&pri[p], te, 20, no, &wr, &wo);
                    printf("P0V2P level=%s seed=%llu const_change_ppm=%lld prior=%s k=%u outcome=%s prior_rank=%d best_roll20=%lld prior_roll20=%lld noprior_best_roll20=%lld\n", lname(level),
                           (unsigned long long)seed, (long long)ppm, PN[p], KS[ki], o, pr, (long long)br, (long long)wr, (long long)nr);
                    cnt[p][ki][!strcmp(o, "KEPT") ? 0 : !strcmp(o, "CORRECTED") ? 1 : !strcmp(o, "REJECTED_OTHER_TERMS") ? 2 : 3]++; vb[p][ki][nv[p][ki]] = br; vp[p][ki][nv[p][ki]] = wr; vn[p][ki][nv[p][ki]] = nr; nv[p][ki]++; pd0_learner_free(Lp); } }
            dev_world_close(S.w); }
        printf("P0V2P-SUM level=%s prior=wrong k=pre seeds=%d skipped=%d kept/corrected/rejected_other_terms/rejected_to_null=%d/%d/%d/%d median_best_roll20=%lld median_prior_roll20=%lld\n", lname(level), nseed, nskip, cnt[0][NK][0], cnt[0][NK][1], cnt[0][NK][2], cnt[0][NK][3], (long long)median(vb[0][NK], nv[0][NK]), (long long)median(vp[0][NK], nv[0][NK]));
        for (int p = 0; p < 2; p++) for (int ki = 0; ki < NK; ki++)
            printf("P0V2P-SUM level=%s prior=%s k=%u seeds=%d skipped=%d kept/corrected/rejected_other_terms/rejected_to_null=%d/%d/%d/%d median_best_roll20=%lld median_prior_roll20=%lld median_noprior_best_roll20=%lld\n", lname(level), PN[p], KS[ki], nseed, nskip, cnt[p][ki][0], cnt[p][ki][1], cnt[p][ki][2], cnt[p][ki][3], (long long)median(vb[p][ki], nv[p][ki]), (long long)median(vp[p][ki], nv[p][ki]), (long long)median(vn[p][ki], nv[p][ki])); }
    return 0;
}
static int mode_prior21(uint64_t lo, uint64_t hi)
{
    for (int arm = 0; arm < 2; arm++) { g_forget = arm; printf("P0V21P-ARM arm=%s (the lines below are in the C3 format of #397)\n", arm ? "v21" : "v2"); int rc = mode_prior(lo, hi); if (rc) return rc; }
    g_forget = 0; return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: pd0-dev2 flag|regime|prior|detect <seed_lo> <seed_hi>\n"); return 2; }
    uint64_t lo = strtoull(argv[2], NULL, 10), hi = strtoull(argv[3], NULL, 10);
    printf("PD0_DEV2 mode=%s learner=%s seeds=%llu..%llu (public dev seeds; development evidence only)\n", argv[1], LNAME, (unsigned long long)lo, (unsigned long long)hi);
    if (!strcmp(argv[1], "flag")) return mode_flag(lo, hi);
#ifndef DEV_V1
    if (!strcmp(argv[1], "regime")) return mode_regime(lo, hi);
    if (!strcmp(argv[1], "prior")) return mode_prior(lo, hi);
    if (!strcmp(argv[1], "detect")) return mode_detect(lo, hi);
    if (!strcmp(argv[1], "shift")) return mode_shift(lo, hi);
    if (!strcmp(argv[1], "prior21")) return mode_prior21(lo, hi);
#endif
    fprintf(stderr, "unknown mode\n"); return 2;
}
