/* PD-0 world process side (spec 2.2, 2.3, 2.5): describe / reset / step with
 * budgets, the STAND_IN range guard (pd0_guard), the STAND_IN recorder (one
 * PD0REC1 record per call, SHA-256 chained), and the framed pipe server. */
#define PD0_WORLD_IMPL 1
#include "physics0/pd0_world.h"

#include <string.h>

#include "physics0/pd0_guard.h"

int pd0_world_init(pd0_world *w, int level, uint64_t seed) {
    memset(w, 0, sizeof *w);
    if (pd0_gen_desc(level, &w->desc) != 0 || pd0_gen_init(&w->gen, level, seed) != 0) return -1;
    w->seed = seed;
    pd0_stream(&w->noise, seed, "noise");
    pd0_stream(&w->null, seed, "null");
    return 0;
}

void pd0_world_describe(const pd0_world *w, pd0_desc *d) { *d = w->desc; }

static void begin_rec(pd0_world *w, pd0_rec *r, uint8_t kind, uint8_t channel) {
    memset(r, 0, sizeof *r);
    r->n_obs = w->desc.n_obs;
    r->kind = kind;
    r->channel = channel;
    r->seq = w->seq;
    r->episode = w->episode;
    r->step_in_episode = w->step_in_episode;
    r->time_micro = (int64_t)w->step_in_episode * w->desc.dt_micro;
    memcpy(r->prev_hash, w->last_hash, PD0_HASH);
}

/* STAND_IN recorder: seal the record into the chain */
static void seal(pd0_world *w, pd0_rec *r) {
    uint8_t buf[PD0_REC_MAX];
    pd0_rec_encode(r, buf, sizeof buf);
    memcpy(w->last_hash, r->hash, PD0_HASH);
    w->seq++;
}

static int budget_spent(const pd0_world *w) {
    return w->steps_used >= w->desc.budget_steps || w->episodes_used >= w->desc.budget_episodes;
}

void pd0_world_reset(pd0_world *w, const int64_t *vals, uint8_t n_vals, pd0_rec *r) {
    begin_rec(w, r, PD0_KIND_RESET, PD0_CH_NONE);
    for (int i = 0; i < w->desc.n_obs; i++) { r->before[i] = w->s[i]; r->after[i] = w->s[i]; }
    if (budget_spent(w)) { r->status = PD0_BUDGET_EXHAUSTED; seal(w, r); return; }
    w->episodes_used++;
    if (w->in_episode || w->seq) w->episode++;
    r->episode = w->episode;
    if (!pd0_guard_reset_ok(&w->desc, vals, n_vals)) { /* STAND_IN guard: refused, still charged */
        r->status = PD0_REFUSED_RANGE;
        w->in_episode = 0;
        seal(w, r);
        return;
    }
    for (int i = 0; i < w->desc.n_obs; i++) w->s[i] = vals[i];
    w->h = 0;
    w->in_episode = 1;
    w->step_in_episode = 0;
    r->step_in_episode = 0;
    r->time_micro = 0;
    int64_t obs[PD0_MAX_OBS];
    pd0_gen_observe_reset(&w->gen, w->s, obs, &w->noise);
    for (int i = 0; i < w->desc.n_obs; i++) { r->before[i] = obs[i]; r->after[i] = obs[i]; }
    r->status = PD0_OK;
    seal(w, r);
}

void pd0_world_step(pd0_world *w, uint8_t channel, int64_t value, pd0_rec *r) {
    begin_rec(w, r, PD0_KIND_STEP, channel);
    r->requested = value;
    int64_t obs_before[PD0_MAX_OBS];
    pd0_gen_observe_reset(&w->gen, w->s, obs_before, &w->noise);
    for (int i = 0; i < w->desc.n_obs; i++) { r->before[i] = obs_before[i]; r->after[i] = obs_before[i]; }
    if (budget_spent(w) || !w->in_episode) { r->status = PD0_BUDGET_EXHAUSTED; seal(w, r); return; }
    w->steps_used++;
    if (!pd0_guard_step_ok(&w->desc, channel, value)) { /* STAND_IN guard: refused, still charged */
        r->status = PD0_REFUSED_RANGE;
        seal(w, r);
        return;
    }
    int64_t u[PD0_MAX_CH] = {0};
    if (channel != PD0_CH_NONE) u[channel] = value;
    r->applied = channel == PD0_CH_NONE ? 0 : value;
    int64_t ns[PD0_MAX_OBS], nh, obs[PD0_MAX_OBS];
    int bad = pd0_gen_step(&w->gen, w->s, w->h, u, w->desc.dt_micro, ns, &nh, obs, &w->noise, &w->null);
    w->step_in_episode++;
    if (bad) {
        r->status = PD0_OUT_OF_BOUNDS; /* nothing revealed: after == before */
        w->in_episode = 0;
        seal(w, r);
        return;
    }
    for (int i = 0; i < w->desc.n_obs; i++) { w->s[i] = ns[i]; r->after[i] = obs[i]; }
    w->h = nh;
    r->status = PD0_OK;
    if (w->step_in_episode >= w->desc.episode_max_steps) { r->status = PD0_EPISODE_END; w->in_episode = 0; }
    seal(w, r);
}

size_t pd0_world_handle(pd0_world *w, const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
    if (len < 1) return 0;
    pd0_rec r;
    switch (req[0]) {
    case PD0_OP_DESCRIBE:
        return len == 1 ? pd0_desc_encode(&w->desc, resp, cap) : 0;
    case PD0_OP_RESET: {
        if (len < 2) return 0;
        uint8_t n = req[1];
        if (n > PD0_MAX_OBS || len != 2 + 8u * n) return 0;
        int64_t vals[PD0_MAX_OBS] = {0};
        for (unsigned i = 0; i < n; i++) vals[i] = (int64_t)pd0_get_u64(req + 2 + 8 * i);
        pd0_world_reset(w, vals, n, &r);
        return pd0_rec_encode(&r, resp, cap);
    }
    case PD0_OP_STEP:
        if (len != 10) return 0;
        pd0_world_step(w, req[1], (int64_t)pd0_get_u64(req + 2), &r);
        return pd0_rec_encode(&r, resp, cap);
    default: return 0;
    }
}

size_t pd0_world_chan(void *ctx, const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
    return pd0_world_handle((pd0_world *)ctx, req, len, resp, cap);
}

int pd0_world_serve(pd0_world *w, int in_fd, int out_fd) {
    uint8_t len4[4], req[PD0_REQ_MAX], resp[PD0_REC_MAX > PD0_DESC_MAX ? PD0_REC_MAX : PD0_DESC_MAX];
    for (;;) {
        if (pd0_read_full(in_fd, len4, 4) != 0) return 0; /* EOF: learner closed */
        uint32_t n = pd0_get_u32(len4);
        if (n == 0 || n > sizeof req) return -1;
        if (pd0_read_full(in_fd, req, n) != 0) return -1;
        size_t rn = pd0_world_handle(w, req, n, resp, sizeof resp);
        pd0_put_u32(len4, (uint32_t)rn);
        if (pd0_write_full(out_fd, len4, 4) != 0) return -1;
        if (rn && pd0_write_full(out_fd, resp, rn) != 0) return -1;
    }
}
