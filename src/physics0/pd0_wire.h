/* PD-0 wire records (spec sections 2.2, 2.3, 2.5): describe record,
 * PD0REC1 observation record with SHA-256 hash chain, and the request bytes
 * of the three calls. Little-endian fixed-width integers, no floating point
 * in any canonical byte. Shared by the world process and the learner; holds
 * no generator knowledge.
 *
 * Request bytes (one request per call, length-prefixed u32 on a pipe):
 *   describe: op u8 = 0
 *   reset:    op u8 = 1, n_obs u8, values i64[n_obs]
 *   step:     op u8 = 2, channel u8 (255 = none), value i64
 * Response: the describe record (op 0) or one PD0REC1 record (op 1, 2). */
#ifndef PD0_WIRE_H
#define PD0_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define PD0_MAX_OBS 4
#define PD0_MAX_CH 2
#define PD0_HASH 32
#define PD0_CH_NONE 255
#define PD0_BOUND 10000000LL   /* |v| <= 10.0 units */
#define PD0_EPISODE_STEPS 100u

enum { PD0_OK = 0, PD0_REFUSED_RANGE = 1, PD0_OUT_OF_BOUNDS = 2, PD0_EPISODE_END = 3, PD0_BUDGET_EXHAUSTED = 4 };
enum { PD0_KIND_RESET = 0, PD0_KIND_STEP = 1 };
enum { PD0_OP_DESCRIBE = 0, PD0_OP_RESET = 1, PD0_OP_STEP = 2 };

typedef struct {
    uint8_t n_obs, n_channels;
    int64_t dt_micro;
    int64_t chan_min[PD0_MAX_CH], chan_max[PD0_MAX_CH];
    int64_t reset_min, reset_max;
    uint32_t episode_max_steps, budget_steps, budget_episodes;
} pd0_desc;

typedef struct {
    uint8_t n_obs, status, kind, channel;
    uint64_t seq;
    uint32_t episode, step_in_episode;
    int64_t time_micro, requested, applied;
    int64_t before[PD0_MAX_OBS], after[PD0_MAX_OBS];
    uint8_t prev_hash[PD0_HASH], hash[PD0_HASH];
} pd0_rec;

#define PD0_DESC_MAX (8 + 2 + 8 + 16 * PD0_MAX_CH + 16 + 12)
#define PD0_REC_SIZE(n_obs) (56u + 16u * (unsigned)(n_obs) + 2u * PD0_HASH)
#define PD0_REC_MAX PD0_REC_SIZE(PD0_MAX_OBS)
#define PD0_REQ_MAX (2 + 8 * PD0_MAX_OBS)

/* encode returns bytes written, 0 on bad input; decode returns 0 ok, -1 refused */
size_t pd0_desc_encode(const pd0_desc *d, uint8_t *out, size_t cap);
int    pd0_desc_decode(const uint8_t *in, size_t len, pd0_desc *d);
/* Fills rec->hash (SHA-256 of every preceding byte) while encoding. */
size_t pd0_rec_encode(pd0_rec *r, uint8_t *out, size_t cap);
/* Refuses unknown version, wrong length or a hash that does not recompute. */
int    pd0_rec_decode(const uint8_t *in, size_t len, pd0_rec *r);
/* 1 if r->prev_hash equals prev->hash (prev NULL: seq 0 with zero prev_hash). */
int    pd0_rec_chained(const pd0_rec *r, const pd0_rec *prev);

size_t pd0_req_describe(uint8_t *out);
size_t pd0_req_reset(uint8_t n_obs, const int64_t *vals, uint8_t *out);
size_t pd0_req_step(uint8_t channel, int64_t value, uint8_t *out);

/* little-endian helpers shared by the law record encoder */
void pd0_put_u16(uint8_t *p, uint16_t v);
void pd0_put_u32(uint8_t *p, uint32_t v);
void pd0_put_u64(uint8_t *p, uint64_t v);
uint16_t pd0_get_u16(const uint8_t *p);
uint32_t pd0_get_u32(const uint8_t *p);
uint64_t pd0_get_u64(const uint8_t *p);

#endif
