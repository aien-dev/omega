/* PD-0 experiment planner, see pd0_planner.h. Integer arithmetic only. */
#include <stdlib.h>
#include <string.h>
#include "pd0_planner.h"
#include "pd0_rng.h"

__extension__ typedef __int128 p_i128;

#define P_MAGIC "PD0PLN1"

typedef struct {
    pd0_exp exp;
    int64_t d_micro, info_micro, cost_micro;
    int eligible;   /* 0 if the hash is already in the prior set */
} p_cand;

/* ---- helpers ---- */

static int64_t p_unit_in(pd0_rng *r, int64_t lo, int64_t hi) {
    int64_t u = pd0_rng_unit(r);
    p_i128 span = (p_i128)(hi - lo) * u;
    return lo + (int64_t)(span / 1000000);
}

static int p_bounds_ok(const pd0p_bounds *b) {
    if (b->n_obs == 0 || b->n_obs > PD0_MAX_OBS) return 0;
    if (b->n_channels == 0 || b->n_channels > PD0_MAX_CHAN) return 0;
    for (int i = 0; i < b->n_obs; i++) if (b->reset_min[i] > b->reset_max[i]) return 0;
    for (int i = 0; i < b->n_channels; i++) if (b->chan_min[i] > b->chan_max[i]) return 0;
    return 1;
}

static int p_sched_in_bounds(const pd0p_bounds *b, const pd0_exp *e) {
    if (e->n_obs != b->n_obs || e->n_steps == 0 || e->n_steps > PD0_MAX_STEPS) return 0;
    if (b->episode_max_steps && e->n_steps > b->episode_max_steps) return 0;
    for (int i = 0; i < e->n_obs; i++)
        if (e->reset[i] < b->reset_min[i] || e->reset[i] > b->reset_max[i]) return 0;
    for (int s = 0; s < e->n_steps; s++) {
        uint8_t c = e->steps[s].channel;
        if (c >= b->n_channels) return 0;   /* passive step (255) is not emitted by the planner */
        if (e->steps[s].value < b->chan_min[c] || e->steps[s].value > b->chan_max[c]) return 0;
    }
    return 1;
}

static uint8_t p_steps_for(const pd0p_bounds *b, uint32_t budget_steps) {
    uint32_t n = PD0_MAX_STEPS;
    if (b->episode_max_steps && b->episode_max_steps < n) n = b->episode_max_steps;
    if (budget_steps < n) n = budget_steps;
    return (uint8_t)n;
}

static void p_random_sched(pd0_rng *r, const pd0p_bounds *b, uint8_t n_steps, pd0_exp *e) {
    memset(e, 0, sizeof *e);
    e->n_obs = b->n_obs; e->n_steps = n_steps;
    for (int i = 0; i < b->n_obs; i++) e->reset[i] = p_unit_in(r, b->reset_min[i], b->reset_max[i]);
    for (int s = 0; s < n_steps; s++) {
        uint8_t c = (uint8_t)(pd0_rng_next(r) % b->n_channels);
        e->steps[s].channel = c;
        e->steps[s].value = p_unit_in(r, b->chan_min[c], b->chan_max[c]);
    }
}

/* rollout of every hypothesis; D, info proxy, cost; schedule hash */
static void p_evaluate(const pd0p_hyps *h, pd0_exp *e, int64_t *d_out, int64_t *info_out, int64_t *cost_out) {
    int64_t r = h->residual_sd_micro;
    int64_t maxdiff = 0, cost = 0;
    p_i128 info = 0;
    e->n_hyp = h->n_hyp;
    for (int k = 0; k < h->n_hyp; k++) {
        const pd0_rel *rel = h->hyp[k];
        int64_t state[PD0_MAX_VARS], next[PD0_MAX_VARS];
        memset(state, 0, sizeof state);
        for (int i = 0; i < e->n_obs; i++) state[i] = e->reset[i];
        for (int s = 0; s < e->n_steps; s++) {
            pd0_rel_step(rel, state, e->steps[s].channel, e->steps[s].value, next);
            for (int i = 0; i < e->n_obs; i++) e->expected[k][s][i] = next[i];
            memcpy(state, next, sizeof state);
        }
    }
    for (int s = 0; s < e->n_steps; s++) {
        int64_t stepmax = 0;
        for (int a = 0; a < h->n_hyp; a++)
            for (int b2 = a + 1; b2 < h->n_hyp; b2++)
                for (int i = 0; i < e->n_obs; i++) {
                    int64_t d = e->expected[a][s][i] - e->expected[b2][s][i];
                    if (d < 0) d = -d;
                    if (d > stepmax) stepmax = d;
                }
        if (stepmax > maxdiff) maxdiff = stepmax;
        p_i128 q = ((p_i128)stepmax * 1000000) / r;   /* stepmax / r in micro */
        info += (q * q) / 1000000;
        cost += e->steps[s].value < 0 ? -e->steps[s].value : e->steps[s].value;
    }
    *d_out = (int64_t)(((p_i128)maxdiff * 1000000) / r);
    e->divergence_micro = *d_out;
    if (info > INT64_MAX) info = INT64_MAX;
    *info_out = (int64_t)info;
    *cost_out = cost;
    pd0_schedule_hash(e->n_obs, e->reset, e->n_steps, e->steps, e->schedule_hash);
}

/* spec 8 ordering: higher D, then lower cost, then lexicographic hash */
static int p_better(const p_cand *a, const p_cand *b) {
    if (a->eligible != b->eligible) return a->eligible > b->eligible;
    if (a->d_micro != b->d_micro) return a->d_micro > b->d_micro;
    if (a->cost_micro != b->cost_micro) return a->cost_micro < b->cost_micro;
    return memcmp(a->exp.schedule_hash, b->exp.schedule_hash, PD0_HASH) < 0;
}

static void p_sort(p_cand *c, uint32_t n) {   /* insertion sort: n is small and the order must be total */
    for (uint32_t i = 1; i < n; i++) {
        p_cand tmp; memcpy(&tmp, &c[i], sizeof tmp);
        uint32_t j = i;
        while (j > 0 && p_better(&tmp, &c[j - 1])) { memcpy(&c[j], &c[j - 1], sizeof tmp); j--; }
        memcpy(&c[j], &tmp, sizeof tmp);
    }
}

static uint8_t p_outside_middle(const pd0p_domain *dom, const pd0_exp *e) {
    for (int i = 0; i < e->n_obs; i++) {
        int64_t lo = dom->var_min[i], hi = dom->var_max[i];
        if (hi <= lo) continue;
        int64_t q = (hi - lo) / 4, mlo = lo + q, mhi = hi - q;
        if (e->reset[i] < mlo || e->reset[i] > mhi) return 1;
        for (int s = 0; s < e->n_steps; s++)
            if (e->expected[0][s][i] < mlo || e->expected[0][s][i] > mhi) return 1;
    }
    return 0;
}

/* ---- public ---- */

void pd0p_prior_add(pd0p_prior *p, const uint8_t hash[PD0_HASH]) {
    if (p->n < PD0P_MAX_PRIOR) memcpy(p->hash[p->n++], hash, PD0_HASH);
}

int pd0p_prior_has(const pd0p_prior *p, const uint8_t hash[PD0_HASH]) {
    if (!p) return 0;
    for (uint32_t i = 0; i < p->n; i++) if (memcmp(p->hash[i], hash, PD0_HASH) == 0) return 1;
    return 0;
}

int pd0p_validate(const pd0p_bounds *b, const pd0p_prior *prior, const pd0_exp *e) {
    uint8_t h[PD0_HASH];
    if (!p_bounds_ok(b) || e->n_obs != b->n_obs) return PD0P_ERR_INPUT;
    pd0_schedule_hash(e->n_obs, e->reset, e->n_steps, e->steps, h);
    if (memcmp(h, e->schedule_hash, PD0_HASH) != 0) return PD0P_ERR_INPUT;
    if (!p_sched_in_bounds(b, e)) return PD0P_ERR_BOUNDS;
    if (pd0p_prior_has(prior, e->schedule_hash)) return PD0P_ERR_DUPLICATE;
    return PD0P_OK;
}

int pd0p_propose(const pd0p_bounds *b, const pd0p_domain *dom, const pd0p_hyps *h,
                 uint32_t budget_steps, const pd0p_prior *prior, uint64_t seed, pd0p_proposal *out) {
    memset(out, 0, sizeof *out);
    out->seed = seed;
    if (!p_bounds_ok(b) || !h || h->n_hyp < 2 || h->n_hyp > PD0_MAX_HYP || h->residual_sd_micro <= 0)
        return out->status = PD0P_ERR_INPUT;
    for (int k = 0; k < h->n_hyp; k++)
        if (!h->hyp[k] || h->hyp[k]->n_vars < b->n_obs || h->hyp[k]->n_channels < b->n_channels)
            return out->status = PD0P_ERR_INPUT;
    if (budget_steps < 2) return out->status = PD0P_ERR_BUDGET;
    uint8_t n_steps = p_steps_for(b, budget_steps);

    p_cand *c = calloc(PD0P_CANDIDATES, sizeof *c);
    if (!c) return out->status = PD0P_ERR_INPUT;
    pd0_rng rng; pd0_rng_stream(&rng, seed, "plan");
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < PD0P_CANDIDATES; i++) {
        p_random_sched(&rng, b, n_steps, &c[i].exp);
        p_evaluate(h, &c[i].exp, &c[i].d_micro, &c[i].info_micro, &c[i].cost_micro);
        c[i].eligible = !pd0p_prior_has(prior, c[i].exp.schedule_hash);
        if (!c[i].eligible) skipped++;
    }
    p_sort(c, PD0P_CANDIDATES);
    uint32_t refined = 0;
    for (uint32_t t = 0; t < PD0P_REFINE_TOP && t < PD0P_CANDIDATES; t++) {
        for (uint32_t round = 0; round < PD0P_REFINE_ROUNDS; round++) {
            p_cand m; memcpy(&m, &c[t], sizeof m);
            uint32_t pos = (uint32_t)(pd0_rng_next(&rng) % (uint32_t)(b->n_obs + n_steps));
            if (pos < b->n_obs) m.exp.reset[pos] = p_unit_in(&rng, b->reset_min[pos], b->reset_max[pos]);
            else {
                uint32_t s = pos - b->n_obs;
                uint8_t ch = (uint8_t)(pd0_rng_next(&rng) % b->n_channels);
                m.exp.steps[s].channel = ch;
                m.exp.steps[s].value = p_unit_in(&rng, b->chan_min[ch], b->chan_max[ch]);
            }
            p_evaluate(h, &m.exp, &m.d_micro, &m.info_micro, &m.cost_micro);
            m.eligible = !pd0p_prior_has(prior, m.exp.schedule_hash);
            if (!m.eligible) { skipped++; continue; }
            if (p_better(&m, &c[t])) { memcpy(&c[t], &m, sizeof m); refined++; }
        }
    }
    p_sort(c, PD0P_REFINE_TOP);
    out->n_candidates = PD0P_CANDIDATES;
    out->n_refined = refined;
    out->n_skipped_duplicate = skipped;
    if (!c[0].eligible) { free(c); return out->status = PD0P_ERR_DUPLICATE; }
    memcpy(&out->exp, &c[0].exp, sizeof out->exp);
    out->divergence_micro = c[0].d_micro;
    out->info_proxy_micro = c[0].info_micro;
    out->cost_micro = c[0].cost_micro;
    out->outside_middle = dom ? p_outside_middle(dom, &out->exp) : 0;
    free(c);
    if (!p_sched_in_bounds(b, &out->exp)) return out->status = PD0P_ERR_BOUNDS;   /* cannot happen; defence */
    if (out->divergence_micro < PD0P_MIN_D_MICRO) return out->status = PD0P_NO_DISCRIMINATING_EXPERIMENT;
    return out->status = PD0P_OK;
}

size_t pd0p_prereg_bytes(const pd0p_proposal *p, uint8_t *out, size_t cap) {
    pd0_exp e;
    if (p->status != PD0P_OK) return 0;
    memcpy(&e, &p->exp, sizeof e);
    return pd0_exp_write(&e, out, cap);
}

static void p_put_i64(uint8_t *o, int64_t v) { pd0_put_u64(o, (uint64_t)v); }

size_t pd0p_plan_write(const pd0p_proposal *p, uint8_t out[PD0P_PLAN_SIZE]) {
    uint8_t *o = out;
    memcpy(o, P_MAGIC, 7); o[7] = 0; o += 8;
    o[0] = 1; o[1] = 0; o += 2;
    *o++ = (uint8_t)(p->status & 0xff);
    pd0_put_u32(o, p->n_candidates); o += 4;
    pd0_put_u32(o, p->n_refined); o += 4;
    pd0_put_u64(o, p->seed); o += 8;
    p_put_i64(o, p->divergence_micro); o += 8;
    p_put_i64(o, p->info_proxy_micro); o += 8;
    p_put_i64(o, p->cost_micro); o += 8;
    *o++ = p->outside_middle;
    memcpy(o, p->exp.schedule_hash, PD0_HASH); o += PD0_HASH;
    return (size_t)(o - out);
}

int pd0p_plan_parse(const uint8_t *in, size_t len, pd0p_proposal *p) {
    if (len != PD0P_PLAN_SIZE || memcmp(in, P_MAGIC "\0", 8) != 0) return -1;
    if (in[8] != 1 || in[9] != 0) return -1;   /* unknown version refused */
    memset(p, 0, sizeof *p);
    p->status = (int8_t)in[10];
    p->n_candidates = pd0_get_u32(in + 11);
    p->n_refined = pd0_get_u32(in + 15);
    p->seed = pd0_get_u64(in + 19);
    p->divergence_micro = (int64_t)pd0_get_u64(in + 27);
    p->info_proxy_micro = (int64_t)pd0_get_u64(in + 35);
    p->cost_micro = (int64_t)pd0_get_u64(in + 43);
    p->outside_middle = in[51];
    memcpy(p->exp.schedule_hash, in + 52, PD0_HASH);
    return 0;
}

int pd0p_passive(const pd0p_bounds *b, uint32_t budget_steps, uint64_t stream_seed, uint32_t index, pd0_exp *out) {
    if (!p_bounds_ok(b)) return PD0P_ERR_INPUT;
    if (budget_steps < 2) return PD0P_ERR_BUDGET;
    pd0_rng r; pd0_rng_stream(&r, stream_seed, "passive");
    r.s ^= (uint64_t)index * 0x9E3779B97F4A7C15ull;   /* index-th schedule of the stream, deterministic */
    p_random_sched(&r, b, p_steps_for(b, budget_steps), out);
    out->n_hyp = 0;
    pd0_schedule_hash(out->n_obs, out->reset, out->n_steps, out->steps, out->schedule_hash);
    return PD0P_OK;
}

uint32_t pd0p_outside_fraction_ppm(const uint8_t *flags, uint32_t n) {
    uint64_t k = 0;
    if (n == 0) return 0;
    for (uint32_t i = 0; i < n; i++) k += flags[i] ? 1u : 0u;
    return (uint32_t)((k * 1000000ull) / n);
}
