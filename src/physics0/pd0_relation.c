#include "physics0/pd0_relation.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "physics0/pd0_rng.h"
#include "sha256.h"

int pd0_relation_valid(const pd0_relation *r) {
    if (!r || r->n_vars < 1 || r->n_vars > PD0_MAX_VARS || r->n_latent > PD0_MAX_LATENT || r->n_latent >= r->n_vars)
        return -1;
    if (r->n_channels < 1 || r->n_channels > PD0_MAX_CH || r->n_eq > r->n_vars) return -1;
    for (int i = 0; i < r->n_eq; i++) {
        const pd0_eq *e = &r->eq[i];
        if (e->target >= r->n_vars || e->n_terms > PD0_MAX_TERMS) return -1;
        for (int j = i + 1; j < r->n_eq; j++)
            if (r->eq[j].target == e->target) return -1;
        for (int t = 0; t < e->n_terms; t++) {
            if (e->t[t].coef == 0) return -1;
            for (int x = r->n_vars; x < PD0_MAX_VARS; x++)
                if (e->t[t].ex[x]) return -1;
            for (int x = PD0_MAX_VARS + r->n_channels; x < PD0_MAX_EX; x++)
                if (e->t[t].ex[x]) return -1;
        }
    }
    return 0;
}

int pd0_relation_size(const pd0_relation *r) {
    int n = r->n_latent;
    for (int i = 0; i < r->n_eq; i++) n += r->eq[i].n_terms;
    return n;
}

int pd0_relation_degree_ok(const pd0_relation *r) {
    for (int i = 0; i < r->n_eq; i++)
        for (int t = 0; t < r->eq[i].n_terms; t++) {
            int d = 0;
            for (int x = 0; x < PD0_MAX_EX; x++) d += r->eq[i].t[t].ex[x];
            if (d > PD0_MAX_DEG) return 0;
        }
    return 1;
}

uint32_t pd0_relation_description_bits(const pd0_relation *r) {
    uint32_t bits = 8u * r->n_latent, per = 8u * ((uint32_t)r->n_vars + r->n_channels) + 24u;
    for (int i = 0; i < r->n_eq; i++) bits += per * r->eq[i].n_terms;
    return bits;
}

int64_t pd0_monomial(const pd0_term *t, const int64_t *vars, const int64_t *u, int n_vars, int n_ch) {
    int64_t m = PD0_MICRO;
    for (int v = 0; v < n_vars; v++)
        for (int p = 0; p < t->ex[v]; p++) m = pd0_mul(m, vars[v]);
    for (int c = 0; c < n_ch; c++)
        for (int p = 0; p < t->ex[PD0_MAX_VARS + c]; p++) m = pd0_mul(m, u[c]);
    return m;
}

void pd0_relation_step(const pd0_relation *r, const int64_t *in, const int64_t *u, int64_t *out) {
    for (int v = 0; v < r->n_vars; v++) out[v] = in[v];
    for (int i = 0; i < r->n_eq; i++) {
        const pd0_eq *e = &r->eq[i];
        int64_t d = 0;
        for (int t = 0; t < e->n_terms; t++)
            d += pd0_mul(e->t[t].coef, pd0_monomial(&e->t[t], in, u, r->n_vars, r->n_channels));
        out[e->target] = in[e->target] + d;
    }
}

void pd0_relation_rollout(const pd0_relation *r, const pd0_sched *s, pd0_traj *out) {
    int64_t cur[PD0_MAX_VARS] = {0}, nxt[PD0_MAX_VARS], u[PD0_MAX_CH];
    out->n_obs = s->n_obs;
    out->n = s->n_steps;
    for (int v = 0; v < s->n_obs; v++) { cur[v] = s->reset[v]; out->s[0][v] = cur[v]; }
    for (int i = 0; i < s->n_steps; i++) {
        memset(u, 0, sizeof u);
        if (s->channel[i] < r->n_channels) u[s->channel[i]] = s->value[i];
        pd0_relation_step(r, cur, u, nxt);
        for (int v = 0; v < r->n_vars; v++) {
            /* keep a diverging rollout finite: clamp far outside the world box */
            if (nxt[v] > 100 * PD0_BOUND) nxt[v] = 100 * PD0_BOUND;
            if (nxt[v] < -100 * PD0_BOUND) nxt[v] = -100 * PD0_BOUND;
            cur[v] = nxt[v];
        }
        for (int v = 0; v < s->n_obs; v++) out->s[i + 1][v] = cur[v];
    }
}

double pd0_nrmse(const pd0_traj *pred, const pd0_traj *truth, int n_traj) {
    double worst = 0.0;
    int n_obs = n_traj > 0 ? truth[0].n_obs : 0;
    for (int j = 0; j < n_obs; j++) {
        double se = 0, sum = 0, sq = 0;
        long n = 0;
        for (int k = 0; k < n_traj; k++)
            for (int i = 1; i <= truth[k].n; i++) {
                double t = (double)truth[k].s[i][j] / 1e6, p = (double)pred[k].s[i][j] / 1e6;
                se += (p - t) * (p - t); sum += t; sq += t * t; n++;
            }
        if (n == 0) return INFINITY;
        double mean = sum / (double)n, var = sq / (double)n - mean * mean;
        double sd = var > 0 ? sqrt(var) : 0.0, rmse = sqrt(se / (double)n);
        double v = sd > 0 ? rmse / sd : (rmse > 0 ? INFINITY : 0.0);
        if (v > worst) worst = v;
    }
    return worst;
}

void pd0_sched_hash(const pd0_sched *s, uint8_t out[PD0_HASH]) {
    uint8_t buf[2 + 8 * PD0_MAX_OBS + 9 * PD0_SCHED_STEPS];
    size_t n = 0;
    buf[n++] = s->n_obs; buf[n++] = s->n_steps;
    for (int v = 0; v < s->n_obs; v++) { pd0_put_u64(buf + n, (uint64_t)s->reset[v]); n += 8; }
    for (int i = 0; i < s->n_steps; i++) { buf[n++] = s->channel[i]; pd0_put_u64(buf + n, (uint64_t)s->value[i]); n += 8; }
    sha256_hash(buf, n, out);
}

static const pd0_term *find_term(const pd0_eq *e, const pd0_term *t) {
    for (int i = 0; i < e->n_terms; i++)
        if (memcmp(e->t[i].ex, t->ex, PD0_MAX_EX) == 0) return &e->t[i];
    return NULL;
}
static const pd0_eq *find_eq(const pd0_relation *r, int target) {
    for (int i = 0; i < r->n_eq; i++)
        if (r->eq[i].target == target) return &r->eq[i];
    return NULL;
}

int pd0_relation_supports(const pd0_relation *r, const pd0_relation *truth) {
    for (int i = 0; i < truth->n_eq; i++) {
        const pd0_eq *e = find_eq(r, truth->eq[i].target);
        if (!e) return 0;
        for (int t = 0; t < truth->eq[i].n_terms; t++)
            if (!find_term(e, &truth->eq[i].t[t])) return 0;
    }
    return 1;
}

int64_t pd0_relation_coef_err_ppm(const pd0_relation *r, const pd0_relation *truth) {
    if (!pd0_relation_supports(r, truth)) return -1;
    int64_t worst = 0;
    for (int i = 0; i < truth->n_eq; i++) {
        const pd0_eq *e = find_eq(r, truth->eq[i].target);
        for (int t = 0; t < truth->eq[i].n_terms; t++) {
            const pd0_term *tt = &truth->eq[i].t[t], *mine = find_term(e, tt);
            pd0_i128 d = (pd0_i128)mine->coef - tt->coef;
            if (d < 0) d = -d;
            int64_t tv = tt->coef < 0 ? -tt->coef : tt->coef;
            int64_t ppm = tv ? (int64_t)(d * 1000000 / tv) : (d ? INT64_MAX : 0);
            if (ppm > worst) worst = ppm;
        }
    }
    return worst;
}

/* ---- PDLAW1 ---- */
static const uint8_t LAW_MAGIC[8] = {'P', 'D', 'L', 'A', 'W', '1', 0, 0};

uint32_t pd0_confidence_ppm(uint32_t p, uint32_t f) {
    return (uint32_t)((1000000ull * (p + 1)) / (uint64_t)(p + f + 2));
}

size_t pd0_claim_text(const pd0_law *l, char *buf, size_t cap) {
    int n = snprintf(buf, cap,
                     "Across %llu observations under %u episodes, %u variables in the reset box [%lld, %lld] micro, "
                     "episode length %u, this relation predicts within %u.%06u (rollout NRMSE, 20 steps). "
                     "Confidence %u ppm. %u exceptions recorded.",
                     (unsigned long long)l->dom.n_observations, l->dom.n_episodes, l->rel.n_vars - l->rel.n_latent,
                     (long long)l->dom.reset_min, (long long)l->dom.reset_max, l->dom.episode_len,
                     l->eps_micro / 1000000u, l->eps_micro % 1000000u, l->confidence_ppm, l->n_exceptions);
    return n < 0 ? 0 : (size_t)n;
}

size_t pd0_law_encode(pd0_law *l, uint8_t *out, size_t cap) {
    if (pd0_relation_valid(&l->rel) != 0 || l->state > PD0_LAW_PROVISIONAL) return 0;
    if (l->n_exceptions > PD0_MAX_EXCEPT || l->n_experiments > PD0_MAX_EXPER) return 0;
    uint8_t *p = out, *end = out + cap;
#define NEED(k) do { if ((size_t)(end - p) < (size_t)(k)) return 0; } while (0)
    NEED(8 + 2 + 1 + PD0_HASH);
    memcpy(p, LAW_MAGIC, 8); p += 8;
    pd0_put_u16(p, 1); p += 2;
    *p++ = l->state;
    uint8_t *law_id_at = p;
    memset(p, 0, PD0_HASH); p += PD0_HASH;
    /* relationship block (spec 7 + n_channels byte, see CALIBRATION.md) */
    const pd0_relation *r = &l->rel;
    int nex = r->n_vars + r->n_channels;
    NEED(10);
    *p++ = r->n_vars; *p++ = r->n_latent; *p++ = r->n_channels;
    pd0_put_u32(p, pd0_relation_description_bits(r)); p += 4;
    pd0_put_u16(p, l->n_refutations); p += 2;
    *p++ = r->n_eq;
    for (int i = 0; i < r->n_eq; i++) {
        const pd0_eq *e = &r->eq[i];
        NEED(3);
        *p++ = e->target; pd0_put_u16(p, e->n_terms); p += 2;
        for (int t = 0; t < e->n_terms; t++) {
            NEED(8 + nex);
            pd0_put_u64(p, (uint64_t)e->t[t].coef); p += 8;
            for (int v = 0; v < r->n_vars; v++) *p++ = e->t[t].ex[v];
            for (int c = 0; c < r->n_channels; c++) *p++ = e->t[t].ex[PD0_MAX_VARS + c];
        }
    }
    /* observed_domain block */
    int n_obs = r->n_vars - r->n_latent;
    NEED(16 * (n_obs + r->n_channels) + 8 + 8 + 4 + 16 + 4 + 8);
    for (int v = 0; v < n_obs; v++) { pd0_put_u64(p, (uint64_t)l->dom.var_min[v]); p += 8; pd0_put_u64(p, (uint64_t)l->dom.var_max[v]); p += 8; }
    for (int c = 0; c < r->n_channels; c++) { pd0_put_u64(p, (uint64_t)l->dom.ch_min[c]); p += 8; pd0_put_u64(p, (uint64_t)l->dom.ch_max[c]); p += 8; }
    pd0_put_u64(p, (uint64_t)l->dom.dt_micro); p += 8;
    pd0_put_u64(p, l->dom.n_observations); p += 8;
    pd0_put_u32(p, l->dom.n_episodes); p += 4;
    pd0_put_u64(p, (uint64_t)l->dom.reset_min); p += 8;
    pd0_put_u64(p, (uint64_t)l->dom.reset_max); p += 8;
    pd0_put_u32(p, l->dom.episode_len); p += 4;
    pd0_put_u64(p, (uint64_t)l->dom.latent_reset); p += 8;
    NEED(4 + 2);
    pd0_put_u32(p, l->confidence_ppm); p += 4;
    pd0_put_u16(p, l->n_exceptions); p += 2;
    for (int i = 0; i < l->n_exceptions; i++) {
        NEED(8 + PD0_HASH + 24);
        pd0_put_u64(p, l->exc[i].seq); p += 8;
        memcpy(p, l->exc[i].hash, PD0_HASH); p += PD0_HASH;
        pd0_put_u64(p, (uint64_t)l->exc[i].predicted); p += 8;
        pd0_put_u64(p, (uint64_t)l->exc[i].observed); p += 8;
        pd0_put_u64(p, (uint64_t)l->exc[i].error_micro); p += 8;
    }
    NEED(2);
    pd0_put_u16(p, l->n_experiments); p += 2;
    for (int i = 0; i < l->n_experiments; i++) {
        NEED(3 * PD0_HASH + 2 + 16);
        memcpy(p, l->exp[i].id, PD0_HASH); p += PD0_HASH;
        *p++ = l->exp[i].kind;
        memcpy(p, l->exp[i].prereg, PD0_HASH); p += PD0_HASH;
        memcpy(p, l->exp[i].outcome, PD0_HASH); p += PD0_HASH;
        *p++ = l->exp[i].result;
        pd0_put_u64(p, l->exp[i].first_seq); p += 8;
        pd0_put_u64(p, l->exp[i].last_seq); p += 8;
    }
    NEED(PD0_HASH + 2);
    memcpy(p, l->chain_root, PD0_HASH); p += PD0_HASH;
    char claim[512];
    size_t cl = pd0_claim_text(l, claim, sizeof claim);
    NEED(2 + cl);
    pd0_put_u16(p, (uint16_t)cl); p += 2;
    memcpy(p, claim, cl); p += cl;
#undef NEED
    size_t total = (size_t)(p - out);
    sha256_hash(out, total, l->law_id);
    memcpy(law_id_at, l->law_id, PD0_HASH);
    return total;
}

int pd0_law_decode(const uint8_t *in, size_t len, pd0_law *l) {
    if (len < 8 + 2 + 1 + PD0_HASH + 10 || memcmp(in, LAW_MAGIC, 8) != 0) return -1;
    if (pd0_get_u16(in + 8) != 1) return -1;
    memset(l, 0, sizeof *l);
    const uint8_t *p = in + 10, *end = in + len;
#define NEED(k) do { if ((size_t)(end - p) < (size_t)(k)) return -1; } while (0)
    l->state = *p++;
    if (l->state > PD0_LAW_PROVISIONAL) return -1;
    memcpy(l->law_id, p, PD0_HASH); p += PD0_HASH;
    pd0_relation *r = &l->rel;
    NEED(10);
    r->n_vars = *p++; r->n_latent = *p++; r->n_channels = *p++;
    uint32_t dbits = pd0_get_u32(p); p += 4;
    l->n_refutations = pd0_get_u16(p); p += 2;
    r->n_eq = *p++;
    if (r->n_vars > PD0_MAX_VARS || r->n_channels > PD0_MAX_CH || r->n_eq > PD0_MAX_VARS) return -1;
    for (int i = 0; i < r->n_eq; i++) {
        NEED(3);
        pd0_eq *e = &r->eq[i];
        e->target = *p++; unsigned nt = pd0_get_u16(p); p += 2;
        if (nt > PD0_MAX_TERMS) return -1;
        e->n_terms = (uint8_t)nt;
        for (unsigned t = 0; t < nt; t++) {
            NEED(8 + r->n_vars + r->n_channels);
            e->t[t].coef = (int64_t)pd0_get_u64(p); p += 8;
            for (int v = 0; v < r->n_vars; v++) e->t[t].ex[v] = *p++;
            for (int c = 0; c < r->n_channels; c++) e->t[t].ex[PD0_MAX_VARS + c] = *p++;
        }
    }
    if (pd0_relation_valid(r) != 0 || dbits != pd0_relation_description_bits(r)) return -1;
    int n_obs = r->n_vars - r->n_latent;
    NEED(16 * (n_obs + r->n_channels) + 8 + 8 + 4 + 16 + 4 + 8 + 4 + 2);
    for (int v = 0; v < n_obs; v++) { l->dom.var_min[v] = (int64_t)pd0_get_u64(p); p += 8; l->dom.var_max[v] = (int64_t)pd0_get_u64(p); p += 8; }
    for (int c = 0; c < r->n_channels; c++) { l->dom.ch_min[c] = (int64_t)pd0_get_u64(p); p += 8; l->dom.ch_max[c] = (int64_t)pd0_get_u64(p); p += 8; }
    l->dom.dt_micro = (int64_t)pd0_get_u64(p); p += 8;
    l->dom.n_observations = pd0_get_u64(p); p += 8;
    l->dom.n_episodes = pd0_get_u32(p); p += 4;
    l->dom.reset_min = (int64_t)pd0_get_u64(p); p += 8;
    l->dom.reset_max = (int64_t)pd0_get_u64(p); p += 8;
    l->dom.episode_len = pd0_get_u32(p); p += 4;
    l->dom.latent_reset = (int64_t)pd0_get_u64(p); p += 8;
    l->confidence_ppm = pd0_get_u32(p); p += 4;
    l->n_exceptions = pd0_get_u16(p); p += 2;
    if (l->n_exceptions > PD0_MAX_EXCEPT) return -1;
    for (int i = 0; i < l->n_exceptions; i++) {
        NEED(8 + PD0_HASH + 24);
        l->exc[i].seq = pd0_get_u64(p); p += 8;
        memcpy(l->exc[i].hash, p, PD0_HASH); p += PD0_HASH;
        l->exc[i].predicted = (int64_t)pd0_get_u64(p); p += 8;
        l->exc[i].observed = (int64_t)pd0_get_u64(p); p += 8;
        l->exc[i].error_micro = (int64_t)pd0_get_u64(p); p += 8;
    }
    NEED(2);
    l->n_experiments = pd0_get_u16(p); p += 2;
    if (l->n_experiments > PD0_MAX_EXPER) return -1;
    for (int i = 0; i < l->n_experiments; i++) {
        NEED(3 * PD0_HASH + 2 + 16);
        memcpy(l->exp[i].id, p, PD0_HASH); p += PD0_HASH;
        l->exp[i].kind = *p++;
        memcpy(l->exp[i].prereg, p, PD0_HASH); p += PD0_HASH;
        memcpy(l->exp[i].outcome, p, PD0_HASH); p += PD0_HASH;
        l->exp[i].result = *p++;
        l->exp[i].first_seq = pd0_get_u64(p); p += 8;
        l->exp[i].last_seq = pd0_get_u64(p); p += 8;
    }
    NEED(PD0_HASH + 2);
    memcpy(l->chain_root, p, PD0_HASH); p += PD0_HASH;
    size_t cl = pd0_get_u16(p); p += 2;
    NEED(cl);
    p += cl;
    if (p != end) return -1;
#undef NEED
    /* law_id must recompute with the field zeroed */
    uint8_t copy[PD0_LAW_MAX], h[PD0_HASH];
    if (len > sizeof copy) return -1;
    memcpy(copy, in, len);
    memset(copy + 11, 0, PD0_HASH);
    sha256_hash(copy, len, h);
    if (memcmp(h, l->law_id, PD0_HASH) != 0) return -1;
    /* eps is not a canonical field; recover it from the claim text is not needed */
    return 0;
}

void pd0_hex(const uint8_t *h, size_t n, char *out) {
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[2 * n] = 0;
}

static const char *STATE_NAMES[4] = {"REJECTED", "REFUTED", "HYPOTHESIS", "PROVISIONAL_LAW"};

size_t pd0_law_json(const pd0_law *l, char *buf, size_t cap) {
    size_t n = 0;
#define EMIT(...) do { int k = snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    char hex[2 * PD0_HASH + 1];
    pd0_hex(l->law_id, PD0_HASH, hex);
    EMIT("{\"state\":\"%s\",\"law_id\":\"%s\",\"relationship\":[", STATE_NAMES[l->state & 3], hex);
    const pd0_relation *r = &l->rel;
    for (int i = 0; i < r->n_eq; i++) {
        const pd0_eq *e = &r->eq[i];
        EMIT("%s{\"target\":\"%s%d\",\"terms\":[", i ? "," : "", e->target < r->n_vars - r->n_latent ? "s" : "h",
             e->target < r->n_vars - r->n_latent ? e->target : e->target - (r->n_vars - r->n_latent));
        for (int t = 0; t < e->n_terms; t++) {
            EMIT("%s{\"coef\":%lld,\"monomial\":\"", t ? "," : "", (long long)e->t[t].coef);
            int any = 0;
            for (int v = 0; v < r->n_vars; v++)
                for (int p = 0; p < e->t[t].ex[v]; p++) { EMIT("%s%s%d", any ? "*" : "", v < r->n_vars - r->n_latent ? "s" : "h", v < r->n_vars - r->n_latent ? v : v - (r->n_vars - r->n_latent)); any = 1; }
            for (int c = 0; c < r->n_channels; c++)
                for (int p = 0; p < e->t[t].ex[PD0_MAX_VARS + c]; p++) { EMIT("%sc%d", any ? "*" : "", c); any = 1; }
            if (!any) EMIT("1");
            EMIT("\"}");
        }
        EMIT("]}");
    }
    EMIT("],\"size\":%d,\"description_bits\":%u,\"n_refutations\":%u,\"observed_domain\":{", pd0_relation_size(r),
         pd0_relation_description_bits(r), l->n_refutations);
    for (int v = 0; v < r->n_vars - r->n_latent; v++) EMIT("\"s%d\":[%lld,%lld],", v, (long long)l->dom.var_min[v], (long long)l->dom.var_max[v]);
    for (int c = 0; c < r->n_channels; c++) EMIT("\"c%d\":[%lld,%lld],", c, (long long)l->dom.ch_min[c], (long long)l->dom.ch_max[c]);
    EMIT("\"dt_micro\":%lld,\"n_observations\":%llu,\"n_episodes\":%u},", (long long)l->dom.dt_micro,
         (unsigned long long)l->dom.n_observations, l->dom.n_episodes);
    EMIT("\"confidence_ppm\":%u,\"n_exceptions\":%u,\"experiments\":%u,", l->confidence_ppm, l->n_exceptions, l->n_experiments);
    char claim[512];
    pd0_claim_text(l, claim, sizeof claim);
    EMIT("\"claim_text\":\"%s\"}", claim);
#undef EMIT
    return n;
}
