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

/* ---- protocol v2 (docs/physics0/PD0_PROTOCOL_V2.md) ---- */
/* score: noise-free truth trajectory. Stateless: no budget, no hash chain, no guard, no RNG draw. */
static size_t handle_score(const pd0_world *w, const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
    if (len < 3 || req[1] != w->desc.n_obs) return 0;
    unsigned no = req[1];
    if (len < 3 + 8u * no) return 0;
    unsigned n = req[2 + 8 * no];
    if (n > PD0_SCORE_STEPS_MAX || len != 3 + 8u * no + 9u * n || cap < 2 + 8u * no * n) return 0;
    int64_t s[PD0_MAX_OBS] = {0}, h = 0, ns[PD0_MAX_OBS], nh, obs[PD0_MAX_OBS];
    for (unsigned j = 0; j < no; j++) s[j] = (int64_t)pd0_get_u64(req + 2 + 8 * j);
    pd0_rng dn, dl;
    pd0_stream(&dn, 1, "x"); pd0_stream(&dl, 1, "y"); /* scratch streams: the returned state never depends on them */
    const uint8_t *sp = req + 3 + 8 * no;
    unsigned done = 0, status = PD0_OK;
    uint8_t *out = resp + 2;
    for (unsigned st = 0; st < n; st++, sp += 9) {
        uint8_t ch = sp[0];
        if (ch != PD0_CH_NONE && ch >= w->desc.n_channels) return 0;
        int64_t u[PD0_MAX_CH] = {0};
        if (ch != PD0_CH_NONE) u[ch] = (int64_t)pd0_get_u64(sp + 1);
        if (pd0_gen_step(&w->gen, s, h, u, w->desc.dt_micro, ns, &nh, obs, &dn, &dl)) { status = PD0_OUT_OF_BOUNDS; break; }
        memcpy(s, ns, sizeof s); h = nh;
        for (unsigned j = 0; j < no; j++) { pd0_put_u64(out, (uint64_t)s[j]); out += 8; }
        done++;
    }
    resp[0] = (uint8_t)status; resp[1] = (uint8_t)done;
    return (size_t)(out - resp);
}

/* shape: the relation shape and the true terms, in PD0SHAP1 form */
static size_t handle_shape(const pd0_world *w, uint8_t *resp, size_t cap) {
    pd0_relation r;
    memset(&r, 0, sizeof r);
    if (pd0_gen_true_relation(&w->gen, w->desc.dt_micro, &r) < 0 || r.n_vars > PD0_MAX_VARS) return 0;
    unsigned nx = r.n_vars + r.n_channels, nt = 0;
    for (int e = 0; e < r.n_eq; e++) nt += r.eq[e].n_terms;
    size_t need = 8 + 4 + 8 + 2 + (size_t)nt * (1 + 8 + nx);
    if (cap < need) return 0;
    uint8_t *p = resp;
    memcpy(p, "PD0SHAP1", 8); p += 8;
    *p++ = r.n_vars; *p++ = r.n_latent; *p++ = r.n_channels; *p++ = r.n_eq;
    pd0_put_u32(p, (uint32_t)pd0_gen_true_size(w->gen.level)); p += 4;
    pd0_put_u32(p, (uint32_t)pd0_relation_size(&r)); p += 4;
    pd0_put_u16(p, (uint16_t)nt); p += 2;
    for (int e = 0; e < r.n_eq; e++) for (int t = 0; t < r.eq[e].n_terms; t++) {
        *p++ = r.eq[e].target;
        pd0_put_u64(p, (uint64_t)r.eq[e].t[t].coef); p += 8;
        for (int i = 0; i < r.n_vars; i++) *p++ = r.eq[e].t[t].ex[i];
        for (int c = 0; c < r.n_channels; c++) *p++ = r.eq[e].t[t].ex[PD0_MAX_VARS + c];
    }
    return (size_t)(p - resp);
}

/* audit: final gate state and refusal counters; free at all times, never counted */
static size_t handle_audit(const pd0_world *w, uint8_t *resp, size_t cap) {
    if (cap < PD0_AUDIT_RESP_SIZE) return 0;
    uint8_t *p = resp;
    memcpy(p, "PD0AUDT1", 8); p += 8;
    *p++ = (uint8_t)(w->final_done ? 1 : 0);
    pd0_put_u32(p, w->n_score_before_final); p += 4;
    pd0_put_u32(p, w->n_shape_before_final); p += 4;
    pd0_put_u32(p, w->n_play_after_final); p += 4;
    pd0_put_u32(p, w->n_dup_final); p += 4;
    memcpy(p, w->final_hash, PD0_HASH); p += PD0_HASH;
    return (size_t)(p - resp);
}

size_t pd0_world_handle(pd0_world *w, const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
    if (len < 1) return 0;
    pd0_rec r;
    switch (req[0]) {
    case PD0_OP_DESCRIBE:
        return len == 1 ? pd0_desc_encode(&w->desc, resp, cap) : 0;
    case PD0_OP_RESET: {
        if (w->final_done) { w->n_play_after_final++; return 0; }
        if (len < 2) return 0;
        uint8_t n = req[1];
        if (n > PD0_MAX_OBS || len != 2 + 8u * n) return 0;
        int64_t vals[PD0_MAX_OBS] = {0};
        for (unsigned i = 0; i < n; i++) vals[i] = (int64_t)pd0_get_u64(req + 2 + 8 * i);
        pd0_world_reset(w, vals, n, &r);
        return pd0_rec_encode(&r, resp, cap);
    }
    case PD0_OP_STEP:
        if (w->final_done) { w->n_play_after_final++; return 0; }
        if (len != 10) return 0;
        pd0_world_step(w, req[1], (int64_t)pd0_get_u64(req + 2), &r);
        return pd0_rec_encode(&r, resp, cap);
    case PD0_OP_SCORE:
        if (!w->final_done) { w->n_score_before_final++; return 0; }
        return handle_score(w, req, len, resp, cap);
    case PD0_OP_SHAPE:
        if (!w->final_done) { w->n_shape_before_final++; return 0; }
        return len == 1 ? handle_shape(w, resp, cap) : 0;
    case PD0_OP_FINAL:
        if (len != 1 + PD0_HASH || cap < 1) return 0;
        if (w->final_done) { w->n_dup_final++; resp[0] = 1; return 1; }   /* duplicate: refused, first hash kept */
        memcpy(w->final_hash, req + 1, PD0_HASH); w->final_done = 1; resp[0] = 0;
        return 1;
    case PD0_OP_AUDIT: return len == 1 ? handle_audit(w, resp, cap) : 0;
    default: return 0;
    }
}

size_t pd0_world_chan(void *ctx, const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
    return pd0_world_handle((pd0_world *)ctx, req, len, resp, cap);
}

int pd0_world_serve(pd0_world *w, int in_fd, int out_fd) {
    static uint8_t req[PD0_REQ_MAX_V2], resp[PD0_SCORE_RESP_MAX > PD0_SHAPE_RESP_MAX ? PD0_SCORE_RESP_MAX : PD0_SHAPE_RESP_MAX];
    uint8_t len4[4];
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
