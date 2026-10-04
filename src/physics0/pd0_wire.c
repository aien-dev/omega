#include "physics0/pd0_wire.h"

#include <string.h>

#include "sha256.h"

void pd0_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void pd0_put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void pd0_put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint16_t pd0_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t pd0_get_u32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
uint64_t pd0_get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static const uint8_t DESC_MAGIC[8] = {'P', 'D', '0', 'D', 'E', 'S', 'C', '2'};
static const uint8_t DESC_MAGIC_V1[8] = {'P', 'D', '0', 'D', 'E', 'S', 'C', '1'}; /* refused */
static const uint8_t REC_MAGIC[8] = {'P', 'D', '0', 'R', 'E', 'C', '1', 0};

size_t pd0_desc_encode(const pd0_desc *d, uint8_t *out, size_t cap) {
    if (!d || d->n_obs < 1 || d->n_obs > PD0_MAX_OBS || d->n_channels < 1 || d->n_channels > PD0_MAX_CH) return 0;
    size_t need = 8 + 2 + 8 + 16u * d->n_channels + 16u * d->n_obs + 12;
    if (cap < need) return 0;
    uint8_t *p = out;
    memcpy(p, DESC_MAGIC, 8); p += 8;
    *p++ = d->n_obs; *p++ = d->n_channels;
    pd0_put_u64(p, (uint64_t)d->dt_micro); p += 8;
    for (unsigned i = 0; i < d->n_channels; i++) { pd0_put_u64(p, (uint64_t)d->chan_min[i]); p += 8; }
    for (unsigned i = 0; i < d->n_channels; i++) { pd0_put_u64(p, (uint64_t)d->chan_max[i]); p += 8; }
    for (unsigned i = 0; i < d->n_obs; i++) { pd0_put_u64(p, (uint64_t)d->reset_min[i]); p += 8; }
    for (unsigned i = 0; i < d->n_obs; i++) { pd0_put_u64(p, (uint64_t)d->reset_max[i]); p += 8; }
    pd0_put_u32(p, d->episode_max_steps); p += 4;
    pd0_put_u32(p, d->budget_steps); p += 4;
    pd0_put_u32(p, d->budget_episodes); p += 4;
    return (size_t)(p - out);
}

int pd0_desc_decode(const uint8_t *in, size_t len, pd0_desc *d) {
    if (len >= 8 && memcmp(in, DESC_MAGIC_V1, 8) == 0) return PD0_DESC_REFUSED_V1;
    if (len < 10 || memcmp(in, DESC_MAGIC, 8) != 0) return -1;
    memset(d, 0, sizeof *d);
    d->n_obs = in[8]; d->n_channels = in[9];
    if (d->n_obs < 1 || d->n_obs > PD0_MAX_OBS || d->n_channels < 1 || d->n_channels > PD0_MAX_CH) return -1;
    size_t need = 8 + 2 + 8 + 16u * d->n_channels + 16u * d->n_obs + 12;
    if (len != need) return -1;
    const uint8_t *p = in + 10;
    d->dt_micro = (int64_t)pd0_get_u64(p); p += 8;
    for (unsigned i = 0; i < d->n_channels; i++) { d->chan_min[i] = (int64_t)pd0_get_u64(p); p += 8; }
    for (unsigned i = 0; i < d->n_channels; i++) { d->chan_max[i] = (int64_t)pd0_get_u64(p); p += 8; }
    for (unsigned i = 0; i < d->n_obs; i++) { d->reset_min[i] = (int64_t)pd0_get_u64(p); p += 8; }
    for (unsigned i = 0; i < d->n_obs; i++) { d->reset_max[i] = (int64_t)pd0_get_u64(p); p += 8; }
    d->episode_max_steps = pd0_get_u32(p); p += 4;
    d->budget_steps = pd0_get_u32(p); p += 4;
    d->budget_episodes = pd0_get_u32(p);
    return 0;
}

static size_t rec_body(const pd0_rec *r, uint8_t *out) {
    uint8_t *p = out;
    memcpy(p, REC_MAGIC, 8); p += 8;
    pd0_put_u16(p, 1); p += 2;
    *p++ = r->n_obs; *p++ = r->status; *p++ = r->kind; *p++ = r->channel;
    *p++ = 0; *p++ = 0;
    pd0_put_u64(p, r->seq); p += 8;
    pd0_put_u32(p, r->episode); p += 4;
    pd0_put_u32(p, r->step_in_episode); p += 4;
    pd0_put_u64(p, (uint64_t)r->time_micro); p += 8;
    pd0_put_u64(p, (uint64_t)r->requested); p += 8;
    pd0_put_u64(p, (uint64_t)r->applied); p += 8;
    for (unsigned i = 0; i < r->n_obs; i++) { pd0_put_u64(p, (uint64_t)r->before[i]); p += 8; }
    for (unsigned i = 0; i < r->n_obs; i++) { pd0_put_u64(p, (uint64_t)r->after[i]); p += 8; }
    memcpy(p, r->prev_hash, PD0_HASH); p += PD0_HASH;
    return (size_t)(p - out);
}

size_t pd0_rec_encode(pd0_rec *r, uint8_t *out, size_t cap) {
    if (!r || r->n_obs < 1 || r->n_obs > PD0_MAX_OBS) return 0;
    size_t need = PD0_REC_SIZE(r->n_obs);
    if (cap < need) return 0;
    size_t n = rec_body(r, out);
    sha256_hash(out, n, r->hash);
    memcpy(out + n, r->hash, PD0_HASH);
    return n + PD0_HASH;
}

int pd0_rec_decode(const uint8_t *in, size_t len, pd0_rec *r) {
    if (len < 12 || memcmp(in, REC_MAGIC, 8) != 0) return -1;
    if (pd0_get_u16(in + 8) != 1) return -1; /* unknown version refused */
    memset(r, 0, sizeof *r);
    r->n_obs = in[10];
    if (r->n_obs < 1 || r->n_obs > PD0_MAX_OBS || len != PD0_REC_SIZE(r->n_obs)) return -1;
    r->status = in[11]; r->kind = in[12]; r->channel = in[13];
    if (in[14] || in[15]) return -1;
    const uint8_t *p = in + 16;
    r->seq = pd0_get_u64(p); p += 8;
    r->episode = pd0_get_u32(p); p += 4;
    r->step_in_episode = pd0_get_u32(p); p += 4;
    r->time_micro = (int64_t)pd0_get_u64(p); p += 8;
    r->requested = (int64_t)pd0_get_u64(p); p += 8;
    r->applied = (int64_t)pd0_get_u64(p); p += 8;
    for (unsigned i = 0; i < r->n_obs; i++) { r->before[i] = (int64_t)pd0_get_u64(p); p += 8; }
    for (unsigned i = 0; i < r->n_obs; i++) { r->after[i] = (int64_t)pd0_get_u64(p); p += 8; }
    memcpy(r->prev_hash, p, PD0_HASH); p += PD0_HASH;
    uint8_t h[PD0_HASH];
    sha256_hash(in, (size_t)(p - in), h);
    if (memcmp(h, p, PD0_HASH) != 0) return -1;
    memcpy(r->hash, h, PD0_HASH);
    return 0;
}

int pd0_rec_chained(const pd0_rec *r, const pd0_rec *prev) {
    static const uint8_t zero[PD0_HASH] = {0};
    if (!prev) return r->seq == 0 && memcmp(r->prev_hash, zero, PD0_HASH) == 0;
    return r->seq == prev->seq + 1 && memcmp(r->prev_hash, prev->hash, PD0_HASH) == 0;
}

size_t pd0_req_describe(uint8_t *out) { out[0] = PD0_OP_DESCRIBE; return 1; }
size_t pd0_req_reset(uint8_t n_obs, const int64_t *vals, uint8_t *out) {
    out[0] = PD0_OP_RESET; out[1] = n_obs;
    for (unsigned i = 0; i < n_obs && i < PD0_MAX_OBS; i++) pd0_put_u64(out + 2 + 8 * i, (uint64_t)vals[i]);
    return 2 + 8u * n_obs;
}
size_t pd0_req_step(uint8_t channel, int64_t value, uint8_t *out) {
    out[0] = PD0_OP_STEP; out[1] = channel;
    pd0_put_u64(out + 2, (uint64_t)value);
    return 10;
}
