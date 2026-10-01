/* fab_loopback.c -- loopback transport and HMAC authenticator. See fab_loopback.h. */
#include "fab_loopback.h"

#include "fab_hmac.h"

#include <string.h>

static FabLoopBox *box_of(FabLoop *l, const AienMachineId *id) {
    for (uint32_t i = 0; i < l->n; i++)
        if (aien_mid_equal(&l->box[i].id, id)) return &l->box[i];
    return NULL;
}

static int box_push(FabLoopBox *b, const uint8_t *msg, size_t len) {
    if (b->count == FAB_LOOP_DEPTH || len > FAB_MSG_MAX) return FAB_E_TRANSPORT;
    FabLoopMsg *m = &b->q[(b->head + b->count) % FAB_LOOP_DEPTH];
    m->len = (uint32_t)len;
    memcpy(m->b, msg, len);
    b->count++;
    return FAB_OK;
}

static int loop_send(void *ctx, const AienMachineId *from, const AienMachineId *to,
                     const uint8_t *msg, size_t len) {
    FabLoop *l = ctx;
    FabLoopBox *src = box_of(l, from), *dst = box_of(l, to);
    if (!src || !dst || len > FAB_MSG_MAX) return FAB_E_TRANSPORT;
    l->last.len = (uint32_t)len;
    memcpy(l->last.b, msg, len);
    if (src->silent) { l->dropped++; return FAB_OK; }
    uint8_t hdr[2 * AIEN_MID_ID_BYTES + 4];
    memcpy(hdr, from->id, AIEN_MID_ID_BYTES);
    memcpy(hdr + AIEN_MID_ID_BYTES, to->id, AIEN_MID_ID_BYTES);
    for (int i = 0; i < 4; i++) hdr[2 * AIEN_MID_ID_BYTES + i] = (uint8_t)(len >> (8 * i));
    sha256_update(&l->transcript, hdr, sizeof hdr);
    sha256_update(&l->transcript, msg, len);
    l->sent++;
    return box_push(dst, msg, len);
}

static int loop_recv(void *ctx, const AienMachineId *self, uint8_t *buf, size_t cap, size_t *len) {
    FabLoopBox *b = box_of(ctx, self);
    if (!b) return FAB_E_TRANSPORT;
    if (b->count == 0) return 0;
    FabLoopMsg *m = &b->q[b->head];
    if (m->len > cap) return FAB_E_TRANSPORT;
    memcpy(buf, m->b, m->len);
    *len = m->len;
    b->head = (b->head + 1) % FAB_LOOP_DEPTH;
    b->count--;
    return 1;
}

void fab_loop_init(FabLoop *l) {
    memset(l, 0, sizeof *l);
    sha256_init(&l->transcript);
    l->transport.ctx = l;
    l->transport.send = loop_send;
    l->transport.recv = loop_recv;
}

int fab_loop_add(FabLoop *l, const AienMachineId *id) {
    if (!l || !id || box_of(l, id)) return FAB_E_ARG;
    if (l->n == FAB_LOOP_NODES) return FAB_E_FULL;
    l->box[l->n++].id = *id;
    return FAB_OK;
}

void fab_loop_silence(FabLoop *l, const AienMachineId *id, int silent) {
    FabLoopBox *b = box_of(l, id);
    if (b) b->silent = silent;
}

int fab_loop_inject(FabLoop *l, const AienMachineId *to, const uint8_t *msg, size_t len) {
    FabLoopBox *b = box_of(l, to);
    return b ? box_push(b, msg, len) : FAB_E_TRANSPORT;
}

uint32_t fab_loop_pending(const FabLoop *l) {
    uint32_t p = 0;
    for (uint32_t i = 0; i < l->n; i++) p += l->box[i].count;
    return p;
}

void fab_loop_transcript(const FabLoop *l, uint8_t out[32]) {
    sha256_ctx c = l->transcript;
    sha256_final(&c, out);
}

static int hmac_sign(void *ctx, const uint8_t *msg, size_t len, uint8_t sig[FAB_SIG_BYTES]) {
    const FabHmacAuth *a = ctx;
    fab_hmac_sig64(a->self_key, msg, len, sig);
    return 0;
}

static int hmac_verify(void *ctx, const AienMachineId *claimed, const uint8_t *msg, size_t len,
                       const uint8_t sig[FAB_SIG_BYTES]) {
    const FabHmacAuth *a = ctx;
    for (uint32_t i = 0; i < a->n; i++) {
        if (!aien_mid_equal(&a->ids[i], claimed)) continue;
        uint8_t t[FAB_SIG_BYTES];
        fab_hmac_sig64(a->keys[i], msg, len, t);
        return fab_ct_equal(t, sig, FAB_SIG_BYTES) ? 0 : -1;
    }
    return -1;
}

int fab_hmac_auth_init(FabHmacAuth *a, const AienMachineId *self, const uint8_t self_key[32],
                        uint32_t n, const AienMachineId *ids, const uint8_t (*keys)[32]) {
    if (!a) return FAB_E_ARG;
    memset(a, 0, sizeof *a);
    if (!self || !self_key || !ids || !keys || n == 0 || n > FAB_HMAC_MAX_PEERS) return FAB_E_ARG;
    a->self = *self;
    memcpy(a->self_key, self_key, 32);
    a->n = n;
    a->ids = ids;
    a->keys = keys;
    a->auth.ctx = a;
    a->auth.sign = hmac_sign;
    a->auth.verify = hmac_verify;
    return FAB_OK;
}
