#include "pd0_calib.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PD0_WORLD_IMPL 1
#include "physics0/pd0_world.h"

static void truth_rollout(const pd0_gen *g, const pd0_desc *d, const pd0_sched *s, pd0_traj *out, int *oob) {
    int64_t cur[PD0_MAX_OBS], nxt[PD0_MAX_OBS], h = 0, nh, obs[PD0_MAX_OBS], u[PD0_MAX_CH];
    pd0_rng dummy = {0};
    out->n_obs = s->n_obs;
    out->n = s->n_steps;
    for (int v = 0; v < s->n_obs; v++) { cur[v] = s->reset[v]; out->s[0][v] = cur[v]; }
    *oob = 0;
    for (int i = 0; i < s->n_steps; i++) {
        memset(u, 0, sizeof u);
        if (s->channel[i] < d->n_channels) u[s->channel[i]] = s->value[i];
        pd0_gen gq = *g;
        gq.sigma_obs = 0; /* noise-free truth */
        int bad = pd0_gen_step(&gq, cur, h, u, d->dt_micro, nxt, &nh, obs, &dummy, &dummy);
        if (bad) { *oob = 1; out->n = (uint8_t)i; return; }
        for (int v = 0; v < s->n_obs; v++) { cur[v] = nxt[v]; out->s[i + 1][v] = cur[v]; }
        h = nh;
    }
}

void pd0_calib_heldout(const pd0_gen *g, const pd0_desc *d, uint64_t seed, const pd0_relation *rel,
                       pd0_calib_result *out) {
    pd0_rng sc;
    pd0_stream(&sc, seed, "score");
    pd0_traj truth[PD0_CALIB_EPISODES], pred[PD0_CALIB_EPISODES], one[PD0_CALIB_EPISODES];
    memset(out, 0, sizeof *out);
    for (int e = 0; e < PD0_CALIB_EPISODES; e++) {
        pd0_sched s;
        memset(&s, 0, sizeof s);
        s.n_obs = d->n_obs;
        s.n_steps = PD0_CALIB_STEPS;
        /* spec 6.1 rev 3: first half in-box = the reset box of each variable, second half the 1.5x extrapolation box */
        const int extrap = e >= PD0_CALIB_EPISODES / 2;
        for (int v = 0; v < d->n_obs; v++) {
            int64_t lo, hi;
            pd0_gen_score_box(g->level, v, extrap, &lo, &hi);
            s.reset[v] = pd0_const(&sc, lo, hi);
        }
        for (int i = 0; i < s.n_steps; i++) {
            s.channel[i] = (uint8_t)(pd0_next(&sc) % d->n_channels);
            s.value[i] = pd0_const(&sc, d->chan_min[s.channel[i]], d->chan_max[s.channel[i]]);
        }
        int oob;
        truth_rollout(g, d, &s, &truth[e], &oob);
        out->truth_oob_episodes += oob;
        pd0_relation_rollout(rel, &s, &pred[e]);
        pred[e].n = truth[e].n;
        /* one-step: predict step i+1 from the true state at i */
        one[e] = truth[e];
        int64_t vars[PD0_MAX_VARS] = {0}, nxt[PD0_MAX_VARS], u[PD0_MAX_CH];
        for (int i = 0; i < truth[e].n; i++) {
            memset(vars, 0, sizeof vars);
            for (int v = 0; v < d->n_obs; v++) vars[v] = truth[e].s[i][v];
            memset(u, 0, sizeof u);
            if (s.channel[i] < rel->n_channels) u[s.channel[i]] = s.value[i];
            pd0_relation_step(rel, vars, u, nxt);
            for (int v = 0; v < d->n_obs; v++) one[e].s[i + 1][v] = nxt[v];
        }
    }
    int half = PD0_CALIB_EPISODES / 2;
    out->nrmse_inbox = pd0_nrmse(pred, truth, half);
    out->nrmse_extrap = pd0_nrmse(pred + half, truth + half, half);
    out->nrmse_onestep = pd0_nrmse(one, truth, PD0_CALIB_EPISODES);
}

int pd0_calib_gather(int level, uint64_t seed, pd0_gather *out) {
    static pd0_world w;
    if (pd0_world_init(&w, level, seed) != 0) return -1;
    pd0_rng rr;
    pd0_stream(&rr, seed, "gather");
    memset(out, 0, sizeof *out);
    out->t = calloc(PD0_GATHER_MAX, sizeof *out->t);
    if (!out->t) return -1;
    pd0_rec r;
    for (;;) {
        int64_t reset[PD0_MAX_OBS];
        for (int v = 0; v < w.desc.n_obs; v++) reset[v] = pd0_const(&rr, w.reset_lo[v], w.reset_hi[v]);
        pd0_world_reset(&w, reset, w.desc.n_obs, &r);
        if (r.status == PD0_BUDGET_EXHAUSTED) break;
        if (r.status == PD0_REFUSED_RANGE) { out->refused++; continue; }
        out->episodes++;
        for (;;) {
            uint8_t ch = (uint8_t)(pd0_next(&rr) % w.desc.n_channels);
            int64_t val = pd0_const(&rr, w.desc.chan_min[ch], w.desc.chan_max[ch]);
            pd0_world_step(&w, ch, val, &r);
            if (r.status == PD0_BUDGET_EXHAUSTED) goto done;
            if (r.status == PD0_OUT_OF_BOUNDS) { out->oob_episodes++; break; }
            if (r.status == PD0_REFUSED_RANGE) { out->refused++; continue; }
            if (out->n < PD0_GATHER_MAX) {
                pd0_transition *t = &out->t[out->n++];
                memcpy(t->before, r.before, sizeof t->before);
                memcpy(t->after, r.after, sizeof t->after);
                memset(t->u, 0, sizeof t->u);
                if (r.channel < PD0_MAX_CH) t->u[r.channel] = r.applied;
                t->episode = r.episode;
            }
            if (r.status == PD0_EPISODE_END) break;
        }
    }
done:
    return 0;
}

/* normal equations with partial pivoting; p <= 64 */
int pd0_lstsq(const double *X, const double *y, int n, int p, double *b) {
    if (p < 1 || p > 64 || n < p) return -1;
    double A[64][65];
    for (int i = 0; i < p; i++) {
        for (int j = 0; j <= p; j++) A[i][j] = 0;
        for (int k = 0; k < n; k++) {
            for (int j = 0; j < p; j++) A[i][j] += X[k * p + i] * X[k * p + j];
            A[i][p] += X[k * p + i] * y[k];
        }
    }
    for (int c = 0; c < p; c++) {
        int piv = c;
        for (int r = c + 1; r < p; r++)
            if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
        if (fabs(A[piv][c]) < 1e-14) return -1;
        if (piv != c)
            for (int j = 0; j <= p; j++) { double t = A[c][j]; A[c][j] = A[piv][j]; A[piv][j] = t; }
        for (int r = 0; r < p; r++) {
            if (r == c) continue;
            double f = A[r][c] / A[c][c];
            for (int j = c; j <= p; j++) A[r][j] -= f * A[c][j];
        }
    }
    for (int i = 0; i < p; i++) b[i] = A[i][p] / A[i][i];
    return 0;
}
