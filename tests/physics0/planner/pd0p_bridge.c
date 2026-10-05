/* See pd0p_bridge.h. Substrate side: links the world and the generator. */
#include <stdlib.h>
#include <string.h>
#define PD0_WORLD_IMPL
#include "physics0/pd0_world.h"
#include "physics0/pd0_gen.h"
#include "physics0/pd0_relation.h"
#include "pd0p_bridge.h"

struct pb_world { pd0_world w; };

pb_world *pb_open(int level, uint64_t seed, pb_bounds *b, pb_rel *truth) {
    pb_world *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    if (pd0_world_init(&p->w, level, seed) != 0) { free(p); return NULL; }
    pd0_desc d; pd0_world_describe(&p->w, &d);
    memset(b, 0, sizeof *b);
    b->n_obs = d.n_obs; b->n_channels = d.n_channels;
    for (int i = 0; i < d.n_channels; i++) { b->chan_min[i] = d.chan_min[i]; b->chan_max[i] = d.chan_max[i]; }
    for (int i = 0; i < d.n_obs; i++) { b->reset_min[i] = d.reset_min[i]; b->reset_max[i] = d.reset_max[i]; }
    b->episode_max_steps = d.episode_max_steps; b->budget_steps = d.budget_steps; b->budget_episodes = d.budget_episodes;
    b->dt_micro = d.dt_micro;
    pd0_relation r;
    if (pd0_gen_true_relation(&p->w.gen, d.dt_micro, &r) != 0) { free(p); return NULL; }
    memset(truth, 0, sizeof *truth);
    truth->n_vars = r.n_vars; truth->n_latent = r.n_latent; truth->n_channels = r.n_channels; truth->n_eq = r.n_eq;
    for (int e = 0; e < r.n_eq && e < PB_MAX_EQ; e++) {
        truth->eq[e].target = r.eq[e].target;
        truth->eq[e].n_terms = r.eq[e].n_terms;
        for (int t = 0; t < r.eq[e].n_terms && t < PB_MAX_TERMS; t++) {
            truth->eq[e].coef[t] = r.eq[e].t[t].coef;
            /* substrate exponent layout: PD0_MAX_VARS variables then PD0_MAX_CH channels;
             * verifier layout: 12 variables then 4 channels */
            for (int v = 0; v < r.n_vars && v < 12; v++) truth->eq[e].expo[t][v] = r.eq[e].t[t].ex[v];
            for (int c = 0; c < r.n_channels && c < 4; c++) truth->eq[e].expo[t][r.n_vars + c] = r.eq[e].t[t].ex[PD0_MAX_VARS + c];   /* verifier layout: channel c at index n_vars + c */
        }
    }
    return p;
}

void pb_close(pb_world *w) { free(w); }

int pb_run(pb_world *p, const pb_sched *s, int64_t after[PB_STEPS][PB_MAX_OBS]) {
    pd0_rec r;
    int64_t vals[PD0_MAX_OBS];
    for (int i = 0; i < s->n_obs && i < PD0_MAX_OBS; i++) vals[i] = s->reset[i];
    pd0_world_reset(&p->w, vals, s->n_obs, &r);
    if (r.status != PD0_OK) return -1;
    int ok = 0;
    for (int st = 0; st < s->n_steps; st++) {
        pd0_world_step(&p->w, s->channel[st], s->value[st], &r);
        if (r.status == PD0_REFUSED_RANGE || r.status == PD0_BUDGET_EXHAUSTED) return ok > 0 ? ok : -1;
        if (r.status == PD0_OUT_OF_BOUNDS) return ok;
        for (int i = 0; i < s->n_obs && i < PD0_MAX_OBS; i++) after[st][i] = r.after[i];
        ok++;
        if (r.status == PD0_EPISODE_END) break;
    }
    return ok;
}

uint32_t pb_steps_used(const pb_world *p) { return p->w.steps_used; }
