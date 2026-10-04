/* PD-0 black-box world (spec section 2) and the learner's channel to it.
 *
 * World side (links pd0_gen.c): pd0_world_* implements describe / reset /
 * step with the bounded intervention rules, budgets, hash chain and the
 * STAND_IN recorder. pd0_world_serve runs the request/response loop over two
 * file descriptors so the world can be its own process (spec 2.4).
 *
 * Learner side (no generator knowledge): pd0_chan is the only handle the
 * learner holds. It sends request bytes and receives record bytes, either
 * through a pipe to the world process or through an in-process function
 * pointer (tests). The learner never sees a pd0_world. */
#ifndef PD0_WORLD_H
#define PD0_WORLD_H

#include <stddef.h>
#include <stdint.h>

#include "physics0/pd0_wire.h"

/* ---- learner side ---- */
typedef size_t (*pd0_chan_fn)(void *ctx, const uint8_t *req, size_t req_len, uint8_t *resp, size_t cap);
typedef struct {
    pd0_chan_fn fn; void *ctx;   /* in-process transport */
    int in_fd, out_fd;           /* or pipe transport (fn == NULL) */
} pd0_chan;

/* framed pipe I/O shared by both ends (u32 length + bytes) */
int pd0_read_full(int fd, uint8_t *buf, size_t n);
int pd0_write_full(int fd, const uint8_t *buf, size_t n);
/* each returns 0 ok, -1 transport or decode failure */
int pd0_chan_describe(pd0_chan *c, pd0_desc *d);
int pd0_chan_reset(pd0_chan *c, uint8_t n_obs, const int64_t *vals, pd0_rec *r);
int pd0_chan_step(pd0_chan *c, uint8_t channel, int64_t value, pd0_rec *r);

/* ---- world side ---- */
#ifdef PD0_WORLD_IMPL
#include "physics0/pd0_gen.h"
typedef struct {
    pd0_gen gen;
    pd0_desc desc;
    int64_t reset_lo[PD0_MAX_OBS], reset_hi[PD0_MAX_OBS]; /* per-variable reset box (the describe record carries variable 0) */
    uint64_t seed;
    pd0_rng noise, null;
    int64_t s[PD0_MAX_OBS], h;
    int in_episode;
    uint32_t step_in_episode, episodes_used, steps_used, episode;
    uint64_t seq;
    uint8_t last_hash[PD0_HASH];
} pd0_world;

int  pd0_world_init(pd0_world *w, int level, uint64_t seed);
void pd0_world_describe(const pd0_world *w, pd0_desc *d);
void pd0_world_reset(pd0_world *w, const int64_t *vals, uint8_t n_vals, pd0_rec *r);
void pd0_world_step(pd0_world *w, uint8_t channel, int64_t value, pd0_rec *r);
/* one request -> one response (bytes); returns response length, 0 refused */
size_t pd0_world_handle(pd0_world *w, const uint8_t *req, size_t len, uint8_t *resp, size_t cap);
/* pd0_chan_fn adapter around pd0_world_handle (ctx = pd0_world*) */
size_t pd0_world_chan(void *ctx, const uint8_t *req, size_t len, uint8_t *resp, size_t cap);
/* serve until EOF on in_fd; frames are u32 length + bytes both ways */
int  pd0_world_serve(pd0_world *w, int in_fd, int out_fd);
#endif

#endif
