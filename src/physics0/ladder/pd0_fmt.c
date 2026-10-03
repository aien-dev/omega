#include "pd0_fmt.h"
#include "pd0_codes.h"
#include "sha256.h"
#include <string.h>

void pd0_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void pd0_put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void pd0_put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint16_t pd0_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t pd0_get_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
uint64_t pd0_get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void put_i64(uint8_t *p, int64_t v) { pd0_put_u64(p, (uint64_t)v); }
static int64_t get_i64(const uint8_t *p) { return (int64_t)pd0_get_u64(p); }

int64_t pd0_mul(int64_t a, int64_t b)
{
    __extension__ typedef __int128 i128;
    i128 p = (i128)a * (i128)b;
    return (int64_t)(p / PD0_MICRO); /* truncation toward zero */
}

/* ---------------- PD0REC1 ---------------- */
static const uint8_t REC_MAGIC[8] = { 'P', 'D', '0', 'R', 'E', 'C', '1', 0 };
size_t pd0_rec_size(uint8_t n_obs) { return 56u + 16u * n_obs + 2u * PD0_HASH; }

size_t pd0_rec_write(const pd0_rec *r, uint8_t *out, size_t cap)
{
    size_t n = pd0_rec_size(r->n_obs);
    if (r->n_obs == 0 || r->n_obs > PD0_MAX_OBS || cap < n) return 0;
    memcpy(out, REC_MAGIC, 8); pd0_put_u16(out + 8, 1); out[10] = r->n_obs; out[11] = r->status;
    out[12] = r->kind; out[13] = r->channel; out[14] = 0; out[15] = 0;
    pd0_put_u64(out + 16, r->seq); pd0_put_u32(out + 24, r->episode); pd0_put_u32(out + 28, r->step_in_episode);
    put_i64(out + 32, r->time_micro); put_i64(out + 40, r->requested); put_i64(out + 48, r->applied);
    size_t o = 56;
    for (int i = 0; i < r->n_obs; i++, o += 8) put_i64(out + o, r->before[i]);
    for (int i = 0; i < r->n_obs; i++, o += 8) put_i64(out + o, r->after[i]);
    memcpy(out + o, r->prev_hash, PD0_HASH); o += PD0_HASH;
    sha256_hash(out, o, out + o);
    memcpy(((pd0_rec *)r)->record_hash, out + o, PD0_HASH);
    return n;
}

int pd0_rec_parse(const uint8_t *in, size_t len, pd0_rec *r, size_t *used)
{
    if (len < 16) return PD0V_TRUNCATED;
    if (memcmp(in, REC_MAGIC, 8) != 0) return PD0V_BAD_MAGIC;
    if (pd0_get_u16(in + 8) != 1) return PD0V_BAD_VERSION;
    memset(r, 0, sizeof *r);
    r->n_obs = in[10];
    if (r->n_obs == 0 || r->n_obs > PD0_MAX_OBS) return PD0V_BAD_FIELD;
    size_t n = pd0_rec_size(r->n_obs);
    if (len < n) return PD0V_TRUNCATED;
    r->status = in[11]; r->kind = in[12]; r->channel = in[13];
    if (r->status > PD0_ST_BUDGET_EXHAUSTED || r->kind > 1) return PD0V_BAD_FIELD;
    r->seq = pd0_get_u64(in + 16); r->episode = pd0_get_u32(in + 24); r->step_in_episode = pd0_get_u32(in + 28);
    r->time_micro = get_i64(in + 32); r->requested = get_i64(in + 40); r->applied = get_i64(in + 48);
    size_t o = 56;
    for (int i = 0; i < r->n_obs; i++, o += 8) r->before[i] = get_i64(in + o);
    for (int i = 0; i < r->n_obs; i++, o += 8) r->after[i] = get_i64(in + o);
    memcpy(r->prev_hash, in + o, PD0_HASH); o += PD0_HASH;
    uint8_t h[PD0_HASH]; sha256_hash(in, o, h);
    if (memcmp(h, in + o, PD0_HASH) != 0) return PD0V_HASH_MISMATCH;
    memcpy(r->record_hash, in + o, PD0_HASH);
    if (used) *used = n;
    return PD0V_OK;
}

/* ---------------- relationship block ---------------- */
size_t pd0_rel_write(const pd0_rel *r, uint8_t *out, size_t cap)
{
    size_t o = 0; unsigned ne = r->n_vars + r->n_channels;
    if (r->n_vars == 0 || r->n_vars > PD0_MAX_VARS || r->n_channels > PD0_MAX_CHAN || r->n_latent >= r->n_vars || r->n_equations > PD0_MAX_EQ) return 0;
    if (cap < 10) return 0;
    out[o++] = r->n_vars; out[o++] = r->n_latent; out[o++] = r->n_channels;
    pd0_put_u32(out + o, r->description_bits); o += 4;
    pd0_put_u16(out + o, r->n_refutations); o += 2;
    out[o++] = r->n_equations;
    for (int e = 0; e < r->n_equations; e++) {
        const pd0_eq *q = &r->eq[e];
        if (q->n_terms > PD0_MAX_TERMS) return 0;
        if (cap < o + 3 + (size_t)q->n_terms * (8 + ne)) return 0;
        out[o++] = q->target; pd0_put_u16(out + o, q->n_terms); o += 2;
        for (int t = 0; t < q->n_terms; t++) {
            put_i64(out + o, q->coef[t]); o += 8;
            memcpy(out + o, q->expo[t], ne); o += ne;
        }
    }
    return o;
}

int pd0_rel_parse(const uint8_t *in, size_t len, pd0_rel *r, size_t *used)
{
    size_t o = 0;
    if (len < 10) return PD0V_TRUNCATED;
    memset(r, 0, sizeof *r);
    r->n_vars = in[o++]; r->n_latent = in[o++]; r->n_channels = in[o++];
    r->description_bits = pd0_get_u32(in + o); o += 4;
    r->n_refutations = pd0_get_u16(in + o); o += 2;
    r->n_equations = in[o++];
    unsigned ne = r->n_vars + r->n_channels;
    if (r->n_vars == 0 || r->n_vars > PD0_MAX_VARS || r->n_channels > PD0_MAX_CHAN || r->n_latent >= r->n_vars || r->n_equations > PD0_MAX_EQ) return PD0V_BAD_FIELD;
    for (int e = 0; e < r->n_equations; e++) {
        pd0_eq *q = &r->eq[e];
        if (len < o + 3) return PD0V_TRUNCATED;
        q->target = in[o++]; q->n_terms = pd0_get_u16(in + o); o += 2;
        if (q->target >= r->n_vars) return PD0V_BAD_FIELD;
        if (q->n_terms > PD0_MAX_TERMS) return PD0V_TOO_LARGE;
        if (len < o + (size_t)q->n_terms * (8 + ne)) return PD0V_TRUNCATED;
        for (int t = 0; t < q->n_terms; t++) {
            q->coef[t] = get_i64(in + o); o += 8;
            memcpy(q->expo[t], in + o, ne); o += ne;
        }
    }
    if (used) *used = o;
    return PD0V_OK;
}

uint32_t pd0_rel_size(const pd0_rel *r)
{
    uint32_t s = r->n_latent;
    for (int e = 0; e < r->n_equations; e++) for (int t = 0; t < r->eq[e].n_terms; t++) if (r->eq[e].coef[t] != 0) s++;
    return s;
}
uint32_t pd0_rel_bits(const pd0_rel *r)
{
    uint32_t b = 8u * r->n_latent, ne = r->n_vars + r->n_channels;
    for (int e = 0; e < r->n_equations; e++) b += (uint32_t)r->eq[e].n_terms * (8u * ne + 24u);
    return b;
}
int pd0_rel_max_degree(const pd0_rel *r)
{
    int d = 0; unsigned ne = r->n_vars + r->n_channels;
    for (int e = 0; e < r->n_equations; e++) for (int t = 0; t < r->eq[e].n_terms; t++) {
        int s = 0; for (unsigned i = 0; i < ne; i++) s += r->eq[e].expo[t][i];
        if (s > d) d = s;
    }
    return d;
}

static int64_t ipow_micro(int64_t base, uint8_t e)
{
    int64_t acc = PD0_MICRO;
    for (uint8_t i = 0; i < e; i++) acc = pd0_mul(acc, base);
    return acc;
}

void pd0_rel_step(const pd0_rel *r, const int64_t *state, uint8_t chan, int64_t value, int64_t *next)
{
    int64_t inputs[PD0_MAX_VARS + PD0_MAX_CHAN]; unsigned ne = r->n_vars + r->n_channels;
    for (int i = 0; i < r->n_vars; i++) inputs[i] = state[i];
    for (int c = 0; c < r->n_channels; c++) inputs[r->n_vars + c] = (chan == c) ? value : 0;
    for (int i = 0; i < r->n_vars; i++) next[i] = state[i];
    for (int e = 0; e < r->n_equations; e++) {
        const pd0_eq *q = &r->eq[e]; int64_t d = 0;
        for (int t = 0; t < q->n_terms; t++) {
            int64_t m = q->coef[t];
            for (unsigned i = 0; i < ne; i++) if (q->expo[t][i]) m = pd0_mul(m, ipow_micro(inputs[i], q->expo[t][i]));
            d += m;
        }
        next[q->target] = state[q->target] + d;
    }
}

/* ---------------- PDLAW1 ---------------- */
static const uint8_t LAW_MAGIC[8] = { 'P', 'D', 'L', 'A', 'W', '1', 0, 0 };
size_t pd0_dom_write(const pd0_domain *d, uint8_t *o)
{
    size_t p = 0; o[p++] = d->n_obs; o[p++] = d->n_channels;
    for (int i = 0; i < d->n_obs; i++) { put_i64(o + p, d->var_min[i]); p += 8; put_i64(o + p, d->var_max[i]); p += 8; }
    for (int i = 0; i < d->n_channels; i++) { put_i64(o + p, d->chan_min[i]); p += 8; put_i64(o + p, d->chan_max[i]); p += 8; }
    put_i64(o + p, d->dt_micro); p += 8; pd0_put_u64(o + p, d->n_observations); p += 8; pd0_put_u32(o + p, d->n_episodes); p += 4;
    put_i64(o + p, d->reset_min); p += 8; put_i64(o + p, d->reset_max); p += 8; pd0_put_u32(o + p, d->episode_len); p += 4; put_i64(o + p, d->latent_reset); p += 8;
    return p;
}
static int dom_parse(const uint8_t *i, size_t n, pd0_domain *d, size_t *used)
{
    size_t p = 0; if (n < 2) return PD0V_TRUNCATED;
    memset(d, 0, sizeof *d); d->n_obs = i[p++]; d->n_channels = i[p++];
    if (d->n_obs == 0 || d->n_obs > PD0_MAX_OBS || d->n_channels > PD0_MAX_CHAN) return PD0V_BAD_FIELD;
    size_t need = 2 + 16u * d->n_obs + 16u * d->n_channels + 8 + 8 + 4 + 8 + 8 + 4 + 8;
    if (n < need) return PD0V_TRUNCATED;
    for (int k = 0; k < d->n_obs; k++) { d->var_min[k] = get_i64(i + p); p += 8; d->var_max[k] = get_i64(i + p); p += 8; }
    for (int k = 0; k < d->n_channels; k++) { d->chan_min[k] = get_i64(i + p); p += 8; d->chan_max[k] = get_i64(i + p); p += 8; }
    d->dt_micro = get_i64(i + p); p += 8; d->n_observations = pd0_get_u64(i + p); p += 8; d->n_episodes = pd0_get_u32(i + p); p += 4;
    d->reset_min = get_i64(i + p); p += 8; d->reset_max = get_i64(i + p); p += 8; d->episode_len = pd0_get_u32(i + p); p += 4; d->latent_reset = get_i64(i + p); p += 8;
    *used = p; return PD0V_OK;
}

size_t pd0_law_write(pd0_law *l, uint8_t *out, size_t cap)
{
    size_t o = 0;
    if (cap < 11 + PD0_HASH || l->n_exceptions > PD0_MAX_EXC || l->n_experiments > PD0_MAX_EXP || l->claim_len > PD0_MAX_CLAIM) return 0;
    memcpy(out, LAW_MAGIC, 8); o = 8; pd0_put_u16(out + o, 1); o += 2; out[o++] = l->state;
    size_t id_off = o; memset(out + o, 0, PD0_HASH); o += PD0_HASH;
    size_t n = pd0_rel_write(&l->rel, out + o, cap - o); if (!n) return 0; o += n;
    if (cap < o + 400) return 0;
    o += pd0_dom_write(&l->dom, out + o);
    pd0_put_u32(out + o, l->confidence_ppm); o += 4;
    pd0_put_u16(out + o, (uint16_t)l->n_exceptions); o += 2;
    if (cap < o + (size_t)l->n_exceptions * 64 + (size_t)l->n_experiments * 115 + 40 + l->claim_len) return 0;
    for (uint32_t k = 0; k < l->n_exceptions; k++) {
        const pd0_exception *x = &l->exc[k];
        pd0_put_u64(out + o, x->record_seq); o += 8; memcpy(out + o, x->record_hash, PD0_HASH); o += PD0_HASH;
        put_i64(out + o, x->predicted); o += 8; put_i64(out + o, x->observed); o += 8; put_i64(out + o, x->error_micro); o += 8;
    }
    pd0_put_u16(out + o, (uint16_t)l->n_experiments); o += 2;
    for (uint32_t k = 0; k < l->n_experiments; k++) {
        const pd0_experiment *x = &l->exp[k];
        memcpy(out + o, x->experiment_id, PD0_HASH); o += PD0_HASH; out[o++] = x->kind;
        memcpy(out + o, x->prereg_hash, PD0_HASH); o += PD0_HASH; memcpy(out + o, x->outcome_hash, PD0_HASH); o += PD0_HASH;
        out[o++] = x->result; pd0_put_u64(out + o, x->first_seq); o += 8; pd0_put_u64(out + o, x->last_seq); o += 8;
    }
    memcpy(out + o, l->chain_root, PD0_HASH); o += PD0_HASH;
    pd0_put_u16(out + o, (uint16_t)l->claim_len); o += 2; memcpy(out + o, l->claim, l->claim_len); o += l->claim_len;
    sha256_hash(out, o, l->law_id); memcpy(out + id_off, l->law_id, PD0_HASH);
    return o;
}

int pd0_law_parse(const uint8_t *in, size_t len, pd0_law *l)
{
    size_t o = 0, u = 0; int rc;
    if (len < 11 + PD0_HASH) return PD0V_TRUNCATED;
    if (memcmp(in, LAW_MAGIC, 8) != 0) return PD0V_BAD_MAGIC;
    if (pd0_get_u16(in + 8) != 1) return PD0V_BAD_VERSION;
    memset(l, 0, sizeof *l); o = 10; l->state = in[o++];
    if (l->state > PDLAW_PROVISIONAL_LAW) return PD0V_BAD_FIELD;
    memcpy(l->law_id, in + o, PD0_HASH); o += PD0_HASH;
    if ((rc = pd0_rel_parse(in + o, len - o, &l->rel, &u)) != 0) return rc;
    o += u;
    if ((rc = dom_parse(in + o, len - o, &l->dom, &u)) != 0) return rc;
    o += u;
    if (len < o + 6) return PD0V_TRUNCATED;
    l->confidence_ppm = pd0_get_u32(in + o); o += 4; l->n_exceptions = pd0_get_u16(in + o); o += 2;
    if (l->confidence_ppm > 1000000u) return PD0V_BAD_FIELD;
    if (l->n_exceptions > PD0_MAX_EXC) return PD0V_TOO_LARGE;
    if (len < o + (size_t)l->n_exceptions * 64 + 2) return PD0V_TRUNCATED;
    for (uint32_t k = 0; k < l->n_exceptions; k++) {
        pd0_exception *x = &l->exc[k];
        x->record_seq = pd0_get_u64(in + o); o += 8; memcpy(x->record_hash, in + o, PD0_HASH); o += PD0_HASH;
        x->predicted = get_i64(in + o); o += 8; x->observed = get_i64(in + o); o += 8; x->error_micro = get_i64(in + o); o += 8;
    }
    l->n_experiments = pd0_get_u16(in + o); o += 2;
    if (l->n_experiments > PD0_MAX_EXP) return PD0V_TOO_LARGE;
    if (len < o + (size_t)l->n_experiments * 115 + PD0_HASH + 2) return PD0V_TRUNCATED;
    for (uint32_t k = 0; k < l->n_experiments; k++) {
        pd0_experiment *x = &l->exp[k];
        memcpy(x->experiment_id, in + o, PD0_HASH); o += PD0_HASH; x->kind = in[o++];
        memcpy(x->prereg_hash, in + o, PD0_HASH); o += PD0_HASH; memcpy(x->outcome_hash, in + o, PD0_HASH); o += PD0_HASH;
        x->result = in[o++]; x->first_seq = pd0_get_u64(in + o); o += 8; x->last_seq = pd0_get_u64(in + o); o += 8;
        if (x->kind > 1 || x->result > 1) return PD0V_BAD_FIELD;
    }
    memcpy(l->chain_root, in + o, PD0_HASH); o += PD0_HASH;
    l->claim_len = pd0_get_u16(in + o); o += 2;
    if (l->claim_len > PD0_MAX_CLAIM) return PD0V_TOO_LARGE;
    if (len < o + l->claim_len) return PD0V_TRUNCATED;
    memcpy(l->claim, in + o, l->claim_len); o += l->claim_len;
    /* verify law_id: hash of canonical bytes with the id zeroed */
    uint8_t tmp[4096]; if (o > sizeof tmp) return PD0V_TOO_LARGE;
    memcpy(tmp, in, o); memset(tmp + 11, 0, PD0_HASH);
    uint8_t h[PD0_HASH]; sha256_hash(tmp, o, h);
    if (memcmp(h, l->law_id, PD0_HASH) != 0) return PD0V_LAW_ID_MISMATCH;
    return PD0V_OK;
}

/* ---------------- PD0EXP1 ---------------- */
static const uint8_t EXP_MAGIC[8] = { 'P', 'D', '0', 'E', 'X', 'P', '1', 0 };
void pd0_schedule_hash(uint8_t n_obs, const int64_t *reset, uint8_t n_steps, const pd0_step *steps, uint8_t out[PD0_HASH])
{
    uint8_t buf[8 * PD0_MAX_OBS + 9 * PD0_MAX_STEPS]; size_t o = 0;
    for (int i = 0; i < n_obs; i++, o += 8) put_i64(buf + o, reset[i]);
    for (int s = 0; s < n_steps; s++) { buf[o++] = steps[s].channel; put_i64(buf + o, steps[s].value); o += 8; }
    sha256_hash(buf, o, out);
}
size_t pd0_exp_write(pd0_exp *e, uint8_t *out, size_t cap)
{
    if (e->n_obs == 0 || e->n_obs > PD0_MAX_OBS || e->n_hyp == 0 || e->n_hyp > PD0_MAX_HYP || e->n_steps == 0 || e->n_steps > PD0_MAX_STEPS) return 0;
    size_t need = 16 + 8u * e->n_obs + 9u * e->n_steps + 8u * e->n_hyp * e->n_steps * e->n_obs + 8 + PD0_HASH;
    if (cap < need) return 0;
    size_t o = 0; memcpy(out, EXP_MAGIC, 8); o = 8; pd0_put_u16(out + o, 1); o += 2;
    out[o++] = e->n_obs; out[o++] = e->n_hyp; out[o++] = e->n_steps; out[o++] = 0; out[o++] = 0; out[o++] = 0;
    for (int i = 0; i < e->n_obs; i++, o += 8) put_i64(out + o, e->reset[i]);
    for (int s = 0; s < e->n_steps; s++) { out[o++] = e->steps[s].channel; put_i64(out + o, e->steps[s].value); o += 8; }
    for (int h = 0; h < e->n_hyp; h++) for (int s = 0; s < e->n_steps; s++) for (int i = 0; i < e->n_obs; i++, o += 8) put_i64(out + o, e->expected[h][s][i]);
    put_i64(out + o, e->divergence_micro); o += 8;
    pd0_schedule_hash(e->n_obs, e->reset, e->n_steps, e->steps, e->schedule_hash);
    memcpy(out + o, e->schedule_hash, PD0_HASH); o += PD0_HASH;
    return o;
}
int pd0_exp_parse(const uint8_t *in, size_t len, pd0_exp *e, size_t *used)
{
    if (len < 16) return PD0V_TRUNCATED;
    if (memcmp(in, EXP_MAGIC, 8) != 0) return PD0V_BAD_MAGIC;
    if (pd0_get_u16(in + 8) != 1) return PD0V_BAD_VERSION;
    memset(e, 0, sizeof *e); e->n_obs = in[10]; e->n_hyp = in[11]; e->n_steps = in[12];
    if (e->n_obs == 0 || e->n_obs > PD0_MAX_OBS || e->n_hyp == 0 || e->n_hyp > PD0_MAX_HYP || e->n_steps == 0 || e->n_steps > PD0_MAX_STEPS) return PD0V_BAD_FIELD;
    size_t need = 16 + 8u * e->n_obs + 9u * e->n_steps + 8u * e->n_hyp * e->n_steps * e->n_obs + 8 + PD0_HASH;
    if (len < need) return PD0V_TRUNCATED;
    size_t o = 16;
    for (int i = 0; i < e->n_obs; i++, o += 8) e->reset[i] = get_i64(in + o);
    for (int s = 0; s < e->n_steps; s++) { e->steps[s].channel = in[o++]; e->steps[s].value = get_i64(in + o); o += 8; }
    for (int h = 0; h < e->n_hyp; h++) for (int s = 0; s < e->n_steps; s++) for (int i = 0; i < e->n_obs; i++, o += 8) e->expected[h][s][i] = get_i64(in + o);
    e->divergence_micro = get_i64(in + o); o += 8;
    uint8_t h[PD0_HASH]; pd0_schedule_hash(e->n_obs, e->reset, e->n_steps, e->steps, h);
    if (memcmp(h, in + o, PD0_HASH) != 0) return PD0V_HASH_MISMATCH;
    memcpy(e->schedule_hash, h, PD0_HASH); o += PD0_HASH;
    if (used) *used = o;
    return PD0V_OK;
}

/* ---------------- ledger ---------------- */
static const uint8_t LEDG_MAGIC[8] = { 'P', 'D', '0', 'L', 'E', 'D', 'G', '1' };
size_t pd0_ledg_append(uint8_t *buf, size_t len, size_t cap, uint8_t kind, const uint8_t *payload, uint32_t plen, uint8_t last_hash[PD0_HASH])
{
    size_t need = PD0_LEDG_HDR + plen + PD0_LEDG_TRAILER;
    if (len + need > cap || kind > LEDG_BATCH) return 0;
    uint8_t *o = buf + len;
    memcpy(o, LEDG_MAGIC, 8); pd0_put_u16(o + 8, 1); o[10] = kind; o[11] = 0; pd0_put_u32(o + 12, plen);
    memcpy(o + PD0_LEDG_HDR, payload, plen);
    memcpy(o + PD0_LEDG_HDR + plen, last_hash, PD0_HASH);
    sha256_hash(o, PD0_LEDG_HDR + plen + PD0_HASH, o + PD0_LEDG_HDR + plen + PD0_HASH);
    memcpy(last_hash, o + PD0_LEDG_HDR + plen + PD0_HASH, PD0_HASH);
    return len + need;
}
int pd0_ledg_next(const uint8_t *buf, size_t len, size_t *off, const uint8_t expect_prev[PD0_HASH], pd0_entry *e)
{
    size_t o = *off;
    if (len - o < PD0_LEDG_HDR) return PD0V_TRUNCATED;
    if (memcmp(buf + o, LEDG_MAGIC, 8) != 0) return PD0V_BAD_MAGIC;
    if (pd0_get_u16(buf + o + 8) != 1) return PD0V_BAD_VERSION;
    e->kind = buf[o + 10]; e->payload_len = pd0_get_u32(buf + o + 12);
    if (e->kind > LEDG_BATCH) return PD0V_BAD_FIELD;
    if (len - o < PD0_LEDG_HDR + (size_t)e->payload_len + PD0_LEDG_TRAILER) return PD0V_TRUNCATED;
    e->payload = buf + o + PD0_LEDG_HDR;
    const uint8_t *prev = e->payload + e->payload_len, *hash = prev + PD0_HASH;
    memcpy(e->prev_hash, prev, PD0_HASH); memcpy(e->entry_hash, hash, PD0_HASH);
    if (memcmp(prev, expect_prev, PD0_HASH) != 0) return PD0V_CHAIN_BROKEN;
    uint8_t h[PD0_HASH]; sha256_hash(buf + o, PD0_LEDG_HDR + e->payload_len + PD0_HASH, h);
    if (memcmp(h, hash, PD0_HASH) != 0) return PD0V_HASH_MISMATCH;
    *off = o + PD0_LEDG_HDR + e->payload_len + PD0_LEDG_TRAILER;
    return PD0V_OK;
}

/* payloads */
size_t pd0_tag_write(const pd0_tag *t, uint8_t *o) { pd0_put_u32(o, t->episode); o[4] = t->tag; pd0_put_u32(o + 5, t->batch_id); o[9] = t->origin; return 10; }
int pd0_tag_parse(const uint8_t *i, size_t n, pd0_tag *t) { if (n != 10) return PD0V_TRUNCATED; t->episode = pd0_get_u32(i); t->tag = i[4]; t->batch_id = pd0_get_u32(i + 5); t->origin = i[9]; return (t->tag > TAG_REP || t->origin > 1) ? PD0V_BAD_FIELD : 0; }
size_t pd0_corr_write(const pd0_corr *c, uint8_t *o) { o[0] = c->var_a; o[1] = c->var_b; pd0_put_u32(o + 2, c->n); put_i64(o + 6, c->r_micro); put_i64(o + 14, c->p_micro); pd0_put_u32(o + 22, c->n_pairs); pd0_put_u32(o + 26, c->n_shuffles); pd0_put_u64(o + 30, c->shuffle_seed); return 38; }
int pd0_corr_parse(const uint8_t *i, size_t n, pd0_corr *c) { if (n != 38) return PD0V_TRUNCATED; c->var_a = i[0]; c->var_b = i[1]; c->n = pd0_get_u32(i + 2); c->r_micro = get_i64(i + 6); c->p_micro = get_i64(i + 14); c->n_pairs = pd0_get_u32(i + 22); c->n_shuffles = pd0_get_u32(i + 26); c->shuffle_seed = pd0_get_u64(i + 30); return 0; }
size_t pd0_fals_write(const pd0_falsifier *f, uint8_t *o) { size_t p = 0; memcpy(o, f->candidate, PD0_HASH); p = PD0_HASH; o[p++] = f->n_rivals; for (int k = 0; k < f->n_rivals; k++) { memcpy(o + p, f->rival[k], PD0_HASH); p += PD0_HASH; } put_i64(o + p, f->eps_micro); p += 8; pd0_put_u32(o + p, f->min_trials); p += 4; return p; }
int pd0_fals_parse(const uint8_t *i, size_t n, pd0_falsifier *f) { if (n < PD0_HASH + 1) return PD0V_TRUNCATED; memcpy(f->candidate, i, PD0_HASH); f->n_rivals = i[PD0_HASH]; if (f->n_rivals > PD0_MAX_HYP - 1) return PD0V_BAD_FIELD; size_t p = PD0_HASH + 1; if (n != p + (size_t)f->n_rivals * PD0_HASH + 12) return PD0V_TRUNCATED; for (int k = 0; k < f->n_rivals; k++) { memcpy(f->rival[k], i + p, PD0_HASH); p += PD0_HASH; } f->eps_micro = get_i64(i + p); p += 8; f->min_trials = pd0_get_u32(i + p); return 0; }
size_t pd0_batch_write(const pd0_batch *b, uint8_t *o) { pd0_put_u32(o, b->batch_id); pd0_put_u64(o + 4, b->stream_seed); pd0_put_u32(o + 12, b->n_episodes); return 16; }
int pd0_batch_parse(const uint8_t *i, size_t n, pd0_batch *b) { if (n != 16) return PD0V_TRUNCATED; b->batch_id = pd0_get_u32(i); b->stream_seed = pd0_get_u64(i + 4); b->n_episodes = pd0_get_u32(i + 12); return 0; }
