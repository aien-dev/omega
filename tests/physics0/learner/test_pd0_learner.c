/* Unit tests for the PD-0 learner (Direction 4). Synthetic worlds are written
 * here from the test's own equations (test-only, like tests/physics0/verify/
 * truth.h); the learner library never sees them. */
#include "pd0_learner.h"
#include "pd0_codes.h"
#include "pd0_controls.h"
#include "pd0_score.h"
#include "pd0_rng.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static void desc2(pd0l_desc *d) { memset(d, 0, sizeof *d); d->n_obs = 2; d->n_channels = 1; d->chan_min[0] = -2000000; d->chan_max[0] = 2000000; for (int j = 0; j < 2; j++) { d->reset_min[j] = -2000000; d->reset_max[j] = 2000000; } d->dt_micro = 50000; d->episode_max_steps = 100; d->budget_steps = 3000; d->budget_episodes = 300; }
/* synthetic cubic map: s0 += dt s1 ; s1 += dt(-k s0 - b s0^3 + u) (micro arithmetic, same evaluator family as the checker) */
static void cubic_step(const int64_t *s, int64_t u, int64_t *n) { int64_t dt = 50000, k = 2500000, b = 900000; n[0] = s[0] + pd0_mul(s[1], dt); n[1] = s[1] + pd0_mul(-pd0_mul(k, s[0]) - pd0_mul(b, pd0_mul(s[0], pd0_mul(s[0], s[0]))) + u, dt); }
static int64_t uni(pd0_rng *g, int64_t lo, int64_t hi) { return lo + pd0_mul(hi - lo, pd0_rng_unit(g)); }
/* feed n_ep episodes of 20 steps; tags FIT except every 4th SELECT; also fills held-out transitions from a separate stream */
static void feed(pd0_learner *L, uint64_t seed, uint32_t n_ep, int null_world, pd0_transition *held, uint32_t *n_held, uint32_t cap)
{
    pd0_rng g; pd0_rng_stream(&g, seed, "feed"); pd0_rng nz; pd0_rng_stream(&nz, seed, "null"); uint32_t h = 0;
    for (uint32_t e = 0; e < n_ep + 10; e++) { int64_t st[2], nx[2]; st[0] = uni(&g, -2000000, 2000000); st[1] = uni(&g, -2000000, 2000000); pd0_rec r; memset(&r, 0, sizeof r); r.n_obs = 2; r.kind = PD0_KIND_RESET; r.channel = PD0_CHAN_NONE; r.episode = e; r.before[0] = r.after[0] = st[0]; r.before[1] = r.after[1] = st[1];
        uint8_t tag = (e % 4 == 3) ? TAG_SELECT : TAG_FIT; int holdout = e >= n_ep; if (!holdout) pd0_learner_observe(L, &r, tag);
        for (int s = 0; s < 20; s++) { int64_t u = uni(&g, -2000000, 2000000); if (null_world) { nx[0] = pd0_rng_noise(&nz, 500000); nx[1] = pd0_rng_noise(&nz, 500000); } else cubic_step(st, u, nx);
            if (llabs(nx[0]) > 10000000 || llabs(nx[1]) > 10000000) break;
            memset(&r, 0, sizeof r); r.n_obs = 2; r.kind = PD0_KIND_STEP; r.status = PD0_ST_OK; r.channel = 0; r.episode = e; r.step_in_episode = (uint32_t)s + 1; r.requested = r.applied = u; r.before[0] = st[0]; r.before[1] = st[1]; r.after[0] = nx[0]; r.after[1] = nx[1];
            if (holdout) { if (h < cap) { pd0_transition *t = &held[h++]; t->before[0] = st[0]; t->before[1] = st[1]; t->chan = 0; t->value = u; t->after[0] = nx[0]; t->after[1] = nx[1]; } } else pd0_learner_observe(L, &r, tag);
            st[0] = nx[0]; st[1] = nx[1]; } }
    if (n_held) *n_held = h;
}
static int64_t heldout_nrmse(pd0_rollout_fn roll, pd0_predict_fn pred, void *ctx, const pd0_transition *t, uint32_t n)
{ (void)roll; double se[2] = { 0 }, s[2] = { 0 }, ss[2] = { 0 }; for (uint32_t i = 0; i < n; i++) { int64_t nx[PD0_MAX_VARS]; int64_t st[PD0_MAX_VARS] = { t[i].before[0], t[i].before[1] }; pred(ctx, st, t[i].chan, t[i].value, nx); for (int j = 0; j < 2; j++) { double e = (double)(nx[j] - t[i].after[j]) / 1e6, v = (double)t[i].after[j] / 1e6; se[j] += e * e; s[j] += v; ss[j] += v * v; } }
  double w = 0; for (int j = 0; j < 2; j++) { double rmse = sqrt(se[j] / n), m = s[j] / n, sd = sqrt(fmax(ss[j] / n - m * m, 0)); double v = sd > 1e-9 ? rmse / sd : rmse; if (v > w) w = v; } return (int64_t)(w * 1e6); }

int main(void)
{
    pd0l_desc d; desc2(&d); static pd0_transition held[4096]; uint32_t nh;
    /* 1. determinism: two learners, same records, byte-identical serialisation and report */
    { pd0_learner *A = pd0_learner_new(&d, 7), *B = pd0_learner_new(&d, 7); feed(A, 11, 60, 0, held, &nh, 4096); feed(B, 11, 60, 0, held, &nh, 4096); pd0_learner_fit(A); pd0_learner_fit(B);
      CHECK(pd0_learner_n_candidates(A) == pd0_learner_n_candidates(B), "same candidate count");
      for (int i = 0; i < pd0_learner_n_candidates(A); i++) { uint8_t a[8192], b[8192]; pd0_rel ra = pd0_learner_candidate(A, i)->rel, rb = pd0_learner_candidate(B, i)->rel; size_t na = pd0_rel_write(&ra, a, sizeof a), nb = pd0_rel_write(&rb, b, sizeof b); CHECK(na == nb && na > 0 && !memcmp(a, b, na), "candidate bytes identical"); }
      static char ja[1 << 18], jbuf[1 << 18]; pd0_learner_report(A, ja, sizeof ja); pd0_learner_report(B, jbuf, sizeof jbuf); CHECK(!strcmp(ja, jbuf), "report identical");
      pd0_corr ca, cb; CHECK(pd0_learner_correlation(A, 5, 500, &ca) == 0 && pd0_learner_correlation(B, 5, 500, &cb) == 0 && !memcmp(&ca, &cb, sizeof ca), "correlation identical");
      /* 2. serialisation round trip through PDLAW1 */
      pd0_rel r0 = pd0_learner_candidate(A, 0)->rel, r1; uint8_t buf[8192]; size_t n = pd0_rel_write(&r0, buf, sizeof buf); size_t used = 0; CHECK(n > 0 && pd0_rel_parse(buf, n, &r1, &used) == PD0V_OK && used == n, "PDLAW1 parses");
      CHECK(r1.n_vars == r0.n_vars && r1.n_equations == r0.n_equations && pd0_rel_size(&r1) == pd0_rel_size(&r0) && pd0_rel_bits(&r1) == pd0_rel_bits(&r0) && r1.description_bits == r0.description_bits, "round trip preserves structure and bits");
      { uint8_t buf2[8192]; size_t n2 = pd0_rel_write(&r1, buf2, sizeof buf2); CHECK(n2 == n && !memcmp(buf, buf2, n), "re-serialisation identical"); }
      /* 3. the compact theory beats a memoriser of the same records on held-out transitions */
      const pd0l_candidate *best = pd0_learner_candidate(A, 0); CHECK(!best->is_null, "best is not the null on the cubic world"); CHECK(pd0_rel_size(&best->rel) <= 6, "compact (size <= 6)");
      pd0_rel rc = best->rel; int64_t theory = heldout_nrmse(pd0_rel_rollout_fn, pd0_rel_predict_fn, &rc, held, nh);
      static pd0_transition fitt[4096]; uint32_t nf = 0; { pd0_learner *C = pd0_learner_new(&d, 7); (void)C; pd0_learner_free(C); }
      /* memoriser over the same FIT episodes, rebuilt from the same stream */
      { pd0_rng g; pd0_rng_stream(&g, 11, "feed"); for (uint32_t e = 0; e < 60; e++) { int64_t st[2], nx[2]; st[0] = uni(&g, -2000000, 2000000); st[1] = uni(&g, -2000000, 2000000); for (int s = 0; s < 20; s++) { int64_t u = uni(&g, -2000000, 2000000); cubic_step(st, u, nx); if (llabs(nx[0]) > 10000000 || llabs(nx[1]) > 10000000) break; if (e % 4 != 3 && nf < 4096) { pd0_transition *t = &fitt[nf++]; t->before[0] = st[0]; t->before[1] = st[1]; t->chan = 0; t->value = u; t->after[0] = nx[0]; t->after[1] = nx[1]; } st[0] = nx[0]; st[1] = nx[1]; } } }
      pd0_memoriser m = { fitt, nf, 2 }; int64_t memo = heldout_nrmse(pd0_memoriser_rollout, pd0_memoriser_predict, &m, held, nh);
      printf("held-out one-step NRMSE: theory %lld micro (size %u, %u bits) vs memoriser %lld micro (%u stored numbers)\n", (long long)theory, pd0_rel_size(&best->rel), pd0_rel_bits(&best->rel), (long long)memo, pd0_memoriser_stored_numbers(&m));
      CHECK(theory < 20000, "theory within the in-box bound on held-out");
      CHECK(theory * 4 < memo, "memoriser loses to the compact theory on held-out");
      /* 4. deterministic seeded experiments and a discriminating proposal between candidate and null */
      { int64_t r1v[2], r2v[2]; pd0_step s1[20], s2[20]; pd0_learner_explore(A, 3, r1v, s1, 20); pd0_learner_explore(B, 3, r2v, s2, 20); CHECK(!memcmp(r1v, r2v, sizeof r1v) && !memcmp(s1, s2, sizeof s1), "explore deterministic");
        pd0_rel hy[2]; hy[0] = best->rel; memset(&hy[1], 0, sizeof hy[1]); hy[1].n_vars = 2; hy[1].n_channels = 1; pd0_exp x; int64_t dv = pd0_learner_propose(A, hy, 2, 99, 64, NULL, 0, &x); CHECK(dv >= 3000000, "proposal separates candidate from null by >= 3 sd");
        for (int j = 0; j < 2; j++) CHECK(x.reset[j] >= -2000000 && x.reset[j] <= 2000000, "proposal reset inside box"); for (int s = 0; s < 20; s++) CHECK(x.steps[s].value >= -2000000 && x.steps[s].value <= 2000000, "proposal step inside bounds");
        uint8_t forb[1][PD0_HASH]; memcpy(forb[0], x.schedule_hash, PD0_HASH); pd0_exp y; pd0_learner_propose(A, hy, 2, 99, 64, (const uint8_t (*)[PD0_HASH])forb, 1, &y); CHECK(memcmp(y.schedule_hash, x.schedule_hash, PD0_HASH) != 0, "forbidden schedule is not re-proposed"); }
      pd0_learner_free(A); pd0_learner_free(B); }
    /* 5. withholding: HOLDOUT/TRIAL/REP records are refused by the learner */
    { pd0_learner *A = pd0_learner_new(&d, 1); pd0_rec r; memset(&r, 0, sizeof r); r.n_obs = 2; r.kind = PD0_KIND_STEP; CHECK(pd0_learner_observe(A, &r, TAG_HOLDOUT) == -1 && pd0_learner_observe(A, &r, TAG_TRIAL) == -1 && pd0_learner_observe(A, &r, TAG_REP) == -1, "non-visible tags refused"); CHECK(pd0_learner_n_transitions(A, TAG_FIT) == 0, "nothing stored"); pd0_learner_free(A); }
    /* 6. null world: no candidate ranks above the null */
    { pd0_learner *A = pd0_learner_new(&d, 3); feed(A, 21, 60, 1, held, &nh, 4096); pd0_learner_fit(A); const pd0l_candidate *c0 = pd0_learner_candidate(A, 0);
      /* i.i.d. draws: "pull to the mean" (next = 0) predicts better than "no change", so MDL may rank it above the
       * empty relation; what must hold is that no candidate predicts within the law bounds (the ladder T3 then
       * refuses it, NC-1). The strongest correlation is regression to the mean (var_a == var_b), which T2 accepts. */
      printf("null world: best rank is %s (mdl %.0f bits, select NRMSE %lld micro), %d candidates\n", c0->is_null ? "the null" : "a relation", c0->mdl_bits, (long long)c0->select_nrmse_micro, pd0_learner_n_candidates(A));
      for (int i = 0; i < pd0_learner_n_candidates(A); i++) CHECK(pd0_learner_candidate(A, i)->select_nrmse_micro > 500000, "null world: no candidate predicts within 0.5 sd");
      pd0_corr c; int rc = pd0_learner_correlation(A, 5, 2000, &c); CHECK(rc != 0 || c.var_a == c.var_b, "null world: the only correlation is regression to the mean"); pd0_learner_free(A); }
    printf("checks %d, failures %d\n", checks, fails); printf("PHYSICS0_LEARNER_UNIT: %s\n", fails ? "FAIL" : "PASS"); return fails ? 1 : 0;
}
