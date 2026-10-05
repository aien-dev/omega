/* Learner-side channel to the PD-0 world (spec 2.3, 2.4): request bytes out,
 * record bytes in, over a pipe to the world process or an in-process
 * function. Holds no generator knowledge and opens no file (G5 / I10). */
#include "physics0/pd0_world.h"

#include <unistd.h>

int pd0_read_full(int fd, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, buf + got, n - got);
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}
int pd0_write_full(int fd, const uint8_t *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t k = write(fd, buf + put, n - put);
        if (k <= 0) return -1;
        put += (size_t)k;
    }
    return 0;
}

static size_t chan_call(pd0_chan *c, const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
    if (c->fn) return c->fn(c->ctx, req, len, resp, cap);
    uint8_t len4[4];
    pd0_put_u32(len4, (uint32_t)len);
    if (pd0_write_full(c->out_fd, len4, 4) != 0 || pd0_write_full(c->out_fd, req, len) != 0) return 0;
    if (pd0_read_full(c->in_fd, len4, 4) != 0) return 0;
    uint32_t n = pd0_get_u32(len4);
    if (n == 0 || n > cap) return 0;
    if (pd0_read_full(c->in_fd, resp, n) != 0) return 0;
    return n;
}

int pd0_chan_describe(pd0_chan *c, pd0_desc *d) {
    uint8_t req[PD0_REQ_MAX], resp[PD0_DESC_MAX];
    size_t n = chan_call(c, req, pd0_req_describe(req), resp, sizeof resp);
    return n ? pd0_desc_decode(resp, n, d) : -1;
}
int pd0_chan_reset(pd0_chan *c, uint8_t n_obs, const int64_t *vals, pd0_rec *r) {
    uint8_t req[PD0_REQ_MAX], resp[PD0_REC_MAX];
    size_t n = chan_call(c, req, pd0_req_reset(n_obs, vals, req), resp, sizeof resp);
    return n ? pd0_rec_decode(resp, n, r) : -1;
}
int pd0_chan_step(pd0_chan *c, uint8_t channel, int64_t value, pd0_rec *r) {
    uint8_t req[PD0_REQ_MAX], resp[PD0_REC_MAX];
    size_t n = chan_call(c, req, pd0_req_step(channel, value, req), resp, sizeof resp);
    return n ? pd0_rec_decode(resp, n, r) : -1;
}
