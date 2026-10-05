#include "pd0_controls.h"
#include "pd0_rng.h"
#include <string.h>

uint32_t pd0_null_world(uint64_t seed, uint8_t n_obs, uint32_t n_episodes, uint32_t steps, int64_t reset_min, int64_t reset_max, int64_t chan_min, int64_t chan_max, pd0_rec *out, uint32_t cap)
{
    pd0_rng nz, sch; pd0_rng_stream(&nz, seed, "null"); pd0_rng_stream(&sch, seed, "score");
    uint32_t n = 0; uint64_t seq = 0; uint8_t prev[PD0_HASH] = { 0 }; uint8_t buf[512];
    for (uint32_t e = 0; e < n_episodes; e++) {
        int64_t st[PD0_MAX_OBS];
        for (int j = 0; j < n_obs; j++) st[j] = reset_min + ((reset_max - reset_min) * pd0_rng_unit(&sch)) / PD0_MICRO;
        for (uint32_t s = 0; s <= steps && n < cap; s++) {
            pd0_rec *r = &out[n]; memset(r, 0, sizeof *r); r->n_obs = n_obs; r->seq = seq++; r->episode = e; r->step_in_episode = s; r->time_micro = (int64_t)s * 50000;
            memcpy(r->prev_hash, prev, PD0_HASH);
            if (s == 0) { r->kind = PD0_KIND_RESET; r->channel = PD0_CHAN_NONE; for (int j = 0; j < n_obs; j++) r->before[j] = r->after[j] = st[j]; }
            else { r->kind = PD0_KIND_STEP; r->channel = 0; r->requested = r->applied = chan_min + ((chan_max - chan_min) * pd0_rng_unit(&sch)) / PD0_MICRO;
                for (int j = 0; j < n_obs; j++) { r->before[j] = st[j]; st[j] = pd0_rng_noise(&nz, 500000); r->after[j] = st[j]; }
                if (s == steps) r->status = PD0_ST_EPISODE_END; }
            pd0_rec_write(r, buf, sizeof buf); memcpy(prev, r->record_hash, PD0_HASH); n++; }
    }
    return n;
}
void pd0_shuffle_outcomes(pd0_rec *recs, uint32_t n, uint64_t seed)
{
    pd0_rng g; pd0_rng_stream(&g, seed, "shuffle"); uint32_t idx[65536], m = 0;
    for (uint32_t i = 0; i < n && m < 65536; i++) if (recs[i].kind == PD0_KIND_STEP && recs[i].status == PD0_ST_OK) idx[m++] = i;
    for (uint32_t i = m; i > 1; i--) { uint32_t j = (uint32_t)(pd0_rng_next(&g) % i); int64_t tmp[PD0_MAX_OBS];
        memcpy(tmp, recs[idx[i - 1]].after, sizeof tmp); memcpy(recs[idx[i - 1]].after, recs[idx[j]].after, sizeof tmp); memcpy(recs[idx[j]].after, tmp, sizeof tmp); }
    /* keep before = previous after inside each episode so the record stream is still well formed */
    for (uint32_t i = 1; i < n; i++) if (recs[i].kind == PD0_KIND_STEP && recs[i].episode == recs[i - 1].episode) memcpy(recs[i].before, recs[i - 1].after, sizeof recs[i].before);
}
void pd0_rechain(pd0_rec *recs, uint32_t n)
{
    uint8_t prev[PD0_HASH] = { 0 }, buf[512];
    for (uint32_t i = 0; i < n; i++) { memcpy(recs[i].prev_hash, prev, PD0_HASH); pd0_rec_write(&recs[i], buf, sizeof buf); memcpy(prev, recs[i].record_hash, PD0_HASH); }
}
void pd0_mut_scale_coef(pd0_rel *r, uint8_t target, const uint8_t *expo, int64_t num, int64_t den)
{
    unsigned ne = r->n_vars + r->n_channels;
    for (int e = 0; e < r->n_equations; e++) if (r->eq[e].target == target) for (int k = 0; k < r->eq[e].n_terms; k++) if (!memcmp(r->eq[e].expo[k], expo, ne)) r->eq[e].coef[k] = (r->eq[e].coef[k] * num) / den;
    r->description_bits = pd0_rel_bits(r);
}
void pd0_mut_pad(pd0_rel *r, uint32_t n_extra, int64_t tiny)
{
    unsigned ne = r->n_vars + r->n_channels; uint32_t added = 0;
    for (int e = 0; e < r->n_equations && added < n_extra; e++) { pd0_eq *q = &r->eq[e];
        for (unsigned i = 0; i < ne && added < n_extra && q->n_terms < PD0_MAX_TERMS; i++) for (unsigned j = i; j < ne && added < n_extra && q->n_terms < PD0_MAX_TERMS; j++) {
            uint8_t ex[PD0_MAX_VARS + PD0_MAX_CHAN]; memset(ex, 0, sizeof ex); ex[i]++; ex[j]++; int dup = 0;
            for (int k = 0; k < q->n_terms; k++) if (!memcmp(q->expo[k], ex, ne)) dup = 1; if (dup) continue;
            q->coef[q->n_terms] = tiny; memcpy(q->expo[q->n_terms], ex, sizeof ex); q->n_terms++; added++; } }
    r->description_bits = pd0_rel_bits(r);
}
int pd0_memoriser_predict(void *ctx, const int64_t *state, uint8_t chan, int64_t value, int64_t *next)
{
    const pd0_memoriser *m = ctx; double best = 1e300; uint32_t bi = 0;
    for (uint32_t i = 0; i < m->n; i++) { double d = 0; for (int j = 0; j < m->n_obs; j++) { double x = (double)(state[j] - m->t[i].before[j]); d += x * x; }
        double dv = (double)(value - (m->t[i].chan == chan ? m->t[i].value : 0)); d += dv * dv; if (m->t[i].chan != chan) d += 1e12; if (d < best) { best = d; bi = i; } }
    for (int j = 0; j < m->n_obs; j++) next[j] = state[j] + (m->t[bi].after[j] - m->t[bi].before[j]);
    return 0;
}
int pd0_memoriser_rollout(void *ctx, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out)
{
    const pd0_memoriser *m = ctx; int64_t st[PD0_MAX_OBS], nx[PD0_MAX_OBS]; memcpy(st, init, sizeof(int64_t) * m->n_obs);
    for (uint32_t s = 0; s < n; s++) { pd0_memoriser_predict(ctx, st, steps[s].channel, steps[s].value, nx); memcpy(out + s * m->n_obs, nx, sizeof(int64_t) * m->n_obs); memcpy(st, nx, sizeof nx); }
    return 0;
}
uint32_t pd0_memoriser_stored_numbers(const pd0_memoriser *m) { return m->n * (2u * m->n_obs + 2u); }
