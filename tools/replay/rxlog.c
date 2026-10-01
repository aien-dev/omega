/* rxlog.c -- RXCLOG01 reader/writer and the independent crumb digest. */
#include "replay/rxlog.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w32(uint8_t **p, uint32_t v) { for (int i = 0; i < 4; i++) *(*p)++ = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t **p, uint64_t v) { for (int i = 0; i < 8; i++) *(*p)++ = (uint8_t)(v >> (8 * i)); }

typedef struct { const uint8_t *p; size_t left; int bad; } rd;
static uint32_t r32(rd *r) {
    if (r->left < 4) { r->bad = 1; return 0; }
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | r->p[i];
    r->p += 4; r->left -= 4;
    return v;
}
static uint64_t r64(rd *r) {
    uint64_t lo = r32(r), hi = r32(r);
    return lo | (hi << 32);
}
static void rbytes(rd *r, uint8_t *out, size_t n) {
    if (r->left < n) { r->bad = 1; memset(out, 0, n); return; }
    memcpy(out, r->p, n); r->p += n; r->left -= n;
}

void rxl_init(rxl_log *l, uint32_t flags) {
    memset(l, 0, sizeof *l);
    l->version = RXL_VERSION;
    l->flags = flags;
}

void rxl_free(rxl_log *l) { free(l->recs); memset(l, 0, sizeof *l); }

int rxl_push(rxl_log *l, const rxl_rec *r) {
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        rxl_rec *n = realloc(l->recs, nc * sizeof *n);
        if (!n) return -1;
        l->recs = n; l->cap = nc;
    }
    l->recs[l->n++] = *r;
    return 0;
}

static void enc_io(uint8_t **p, const rxl_io *io) { w32(p, io->id); w32(p, io->gen); w64(p, io->version); w64(p, io->mask); }

size_t rxl_encode(const rxl_rec *r, uint8_t *buf) {
    uint8_t *p = buf;
    switch (r->type) {
    case RXL_CRUMB: {
        const rxl_crumb *k = &r->u.c;
        w64(&p, k->id); w32(&p, k->kind); w32(&p, k->reaction); w32(&p, k->faculty); w32(&p, k->worker);
        w64(&p, k->wake_cause); w64(&p, k->coalesced);
        w32(&p, k->n_inputs);
        for (uint32_t i = 0; i < k->n_inputs && i < RXL_MAX_DEPS; i++) enc_io(&p, &k->inputs[i]);
        w32(&p, k->n_caps);
        for (uint32_t i = 0; i < k->n_caps && i < RXL_MAX_CAPS; i++) {
            w32(&p, k->caps[i].cap_id); w64(&p, k->caps[i].gen); w32(&p, k->caps[i].issuer);
        }
        w32(&p, k->n_outputs);
        for (uint32_t i = 0; i < k->n_outputs && i < RXL_MAX_WRITES; i++) enc_io(&p, &k->outputs[i]);
        w32(&p, (uint32_t)k->reason);
        w32(&p, k->n_parents);
        for (uint32_t i = 0; i < k->n_parents && i < RXL_MAX_PARENTS; i++) w64(&p, k->parents[i]);
        w64(&p, k->t_start); w64(&p, k->t_end); w64(&p, k->episode);
        memcpy(p, k->digest, 32); p += 32;
        break;
    }
    case RXL_INPUT: {
        const rxl_input *in = &r->u.in;
        w64(&p, in->after_crumb); w32(&p, in->cap_id); w64(&p, in->cap_gen); w32(&p, in->n);
        for (uint32_t i = 0; i < in->n && i < RXL_MAX_MUTS; i++) {
            w32(&p, in->m[i].id); w32(&p, in->m[i].gen); w32(&p, in->m[i].field); w64(&p, in->m[i].value);
        }
        break;
    }
    case RXL_CHECKPOINT:
        w64(&p, r->u.ck.through_crumb); w32(&p, r->u.ck.subsystem);
        memcpy(p, r->u.ck.hash, 32); p += 32;
        break;
    case RXL_END:
        w64(&p, r->u.end.n_records);
        memcpy(p, r->u.end.head, 32); p += 32;
        break;
    default:
        return 0;
    }
    return (size_t)(p - buf);
}

static void dec_io(rd *d, rxl_io *io) { io->id = r32(d); io->gen = r32(d); io->version = r64(d); io->mask = r64(d); }

/* Decode one payload; 0 on success, else a reason string in why. */
static int decode(uint32_t type, const uint8_t *buf, size_t len, rxl_rec *r, char *why, size_t wn) {
    rd d = { buf, len, 0 };
    memset(r, 0, sizeof *r);
    r->type = type;
    switch (type) {
    case RXL_CRUMB: {
        rxl_crumb *k = &r->u.c;
        k->id = r64(&d); k->kind = r32(&d); k->reaction = r32(&d); k->faculty = r32(&d); k->worker = r32(&d);
        k->wake_cause = r64(&d); k->coalesced = r64(&d);
        k->n_inputs = r32(&d);
        if (k->n_inputs > RXL_MAX_DEPS) { snprintf(why, wn, "n_inputs %u over limit", k->n_inputs); return -1; }
        for (uint32_t i = 0; i < k->n_inputs; i++) dec_io(&d, &k->inputs[i]);
        k->n_caps = r32(&d);
        if (k->n_caps > RXL_MAX_CAPS) { snprintf(why, wn, "n_caps %u over limit", k->n_caps); return -1; }
        for (uint32_t i = 0; i < k->n_caps; i++) {
            k->caps[i].cap_id = r32(&d); k->caps[i].gen = r64(&d); k->caps[i].issuer = r32(&d);
        }
        k->n_outputs = r32(&d);
        if (k->n_outputs > RXL_MAX_WRITES) { snprintf(why, wn, "n_outputs %u over limit", k->n_outputs); return -1; }
        for (uint32_t i = 0; i < k->n_outputs; i++) dec_io(&d, &k->outputs[i]);
        k->reason = (int32_t)r32(&d);
        k->n_parents = r32(&d);
        if (k->n_parents > RXL_MAX_PARENTS) { snprintf(why, wn, "n_parents %u over limit", k->n_parents); return -1; }
        for (uint32_t i = 0; i < k->n_parents; i++) k->parents[i] = r64(&d);
        k->t_start = r64(&d); k->t_end = r64(&d); k->episode = r64(&d);
        rbytes(&d, k->digest, 32);
        break;
    }
    case RXL_INPUT: {
        rxl_input *in = &r->u.in;
        in->after_crumb = r64(&d); in->cap_id = r32(&d); in->cap_gen = r64(&d); in->n = r32(&d);
        if (in->n > RXL_MAX_MUTS) { snprintf(why, wn, "input count %u over limit", in->n); return -1; }
        for (uint32_t i = 0; i < in->n; i++) {
            in->m[i].id = r32(&d); in->m[i].gen = r32(&d); in->m[i].field = r32(&d); in->m[i].value = r64(&d);
        }
        break;
    }
    case RXL_CHECKPOINT:
        r->u.ck.through_crumb = r64(&d); r->u.ck.subsystem = r32(&d);
        rbytes(&d, r->u.ck.hash, 32);
        break;
    case RXL_END:
        r->u.end.n_records = r64(&d);
        rbytes(&d, r->u.end.head, 32);
        break;
    default:
        snprintf(why, wn, "unknown record type %u", type);
        return -1;
    }
    if (d.bad) { snprintf(why, wn, "payload shorter than its counts imply"); return -1; }
    if (d.left) { snprintf(why, wn, "%zu extra payload bytes", d.left); return -1; }
    return 0;
}

int rxl_read(const char *path, rxl_log *l, char *err, size_t errlen) {
    memset(l, 0, sizeof *l);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return -1; }
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, RXL_MAGIC, 8)) {
        fclose(f); snprintf(err, errlen, "not an RXCLOG01 file"); return -1;
    }
    rd h = { hdr + 8, 8, 0 };
    l->version = r32(&h);
    l->flags = r32(&h);
    if (l->version != RXL_VERSION) {
        fclose(f); snprintf(err, errlen, "unknown version %u", l->version); return -1;
    }
    uint8_t buf[RXL_MAX_PAYLOAD];
    for (;;) {
        uint8_t th[8];
        size_t got = fread(th, 1, 8, f);
        if (got == 0) break;
        if (got != 8) { l->malformed = 1; snprintf(l->why, sizeof l->why, "truncated record header"); break; }
        rd t = { th, 8, 0 };
        uint32_t type = r32(&t), len = r32(&t);
        if (len > RXL_MAX_PAYLOAD) { l->malformed = 1; snprintf(l->why, sizeof l->why, "payload length %u too large", len); break; }
        if (fread(buf, 1, len, f) != len) { l->malformed = 1; snprintf(l->why, sizeof l->why, "truncated payload"); break; }
        rxl_rec r;
        if (decode(type, buf, len, &r, l->why, sizeof l->why)) { l->malformed = 1; break; }
        if (rxl_push(l, &r)) { fclose(f); snprintf(err, errlen, "out of memory"); return -1; }
    }
    fclose(f);
    return 0;
}

int rxl_write(const char *path, const rxl_log *l) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t hdr[16], *p = hdr + 8;
    memcpy(hdr, RXL_MAGIC, 8);
    w32(&p, l->version); w32(&p, l->flags);
    int bad = fwrite(hdr, 1, 16, f) != 16;
    uint8_t buf[RXL_MAX_PAYLOAD + 8];
    for (size_t i = 0; i < l->n && !bad; i++) {
        size_t n = rxl_encode(&l->recs[i], buf + 8);
        uint8_t *q = buf;
        w32(&q, l->recs[i].type); w32(&q, (uint32_t)n);
        bad = fwrite(buf, 1, n + 8, f) != n + 8;
    }
    if (fclose(f)) bad = 1;
    return bad ? -1 : 0;
}

/* The runtime's put32/put64: little-endian, 64-bit as low word then high. */
static void h32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sha256_update(c, b, 4);
}
static void h64(sha256_ctx *c, uint64_t v) { h32(c, (uint32_t)v); h32(c, (uint32_t)(v >> 32)); }

void rxl_crumb_digest(const rxl_crumb *k, const uint8_t (*digest_by_id)[32], uint64_t n_known,
                      uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_CAUSAL_V1", 17);
    h64(&c, k->id);
    h32(&c, k->kind);
    h32(&c, k->reaction);
    h32(&c, k->faculty);
    h64(&c, k->wake_cause);
    h64(&c, k->coalesced);
    h32(&c, k->n_inputs);
    for (uint32_t i = 0; i < k->n_inputs; i++) {
        h32(&c, k->inputs[i].id); h32(&c, k->inputs[i].gen);
        h64(&c, k->inputs[i].version); h64(&c, k->inputs[i].mask);
    }
    h32(&c, k->n_caps);
    for (uint32_t i = 0; i < k->n_caps; i++) {
        h32(&c, k->caps[i].cap_id); h64(&c, k->caps[i].gen); h32(&c, k->caps[i].issuer);
    }
    h32(&c, k->n_outputs);
    for (uint32_t i = 0; i < k->n_outputs; i++) {
        h32(&c, k->outputs[i].id); h32(&c, k->outputs[i].gen);
        h64(&c, k->outputs[i].version); h64(&c, k->outputs[i].mask);
    }
    h32(&c, (uint32_t)k->reason);
    h32(&c, k->n_parents);
    for (uint32_t i = 0; i < k->n_parents; i++) {
        uint64_t p = k->parents[i];
        h64(&c, p);
        if (p >= 1 && p <= n_known && p < k->id) sha256_update(&c, digest_by_id[p - 1], 32);
    }
    sha256_final(&c, out);
}

void rxl_event_digest(const rxl_rec *r, uint8_t out[32]) {
    if (r->type == RXL_CRUMB) { memcpy(out, r->u.c.digest, 32); return; }
    uint8_t buf[RXL_MAX_PAYLOAD];
    size_t n = rxl_encode(r, buf);
    sha256_ctx c;
    sha256_init(&c);
    h32(&c, r->type);
    sha256_update(&c, buf, n);
    sha256_final(&c, out);
}

void rxl_head_step(uint8_t head[32], const rxl_rec *r) {
    uint8_t ev[32];
    rxl_event_digest(r, ev);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"RXCLOG01-HEAD", 13);
    sha256_update(&c, head, 32);
    h32(&c, r->type);
    sha256_update(&c, ev, 32);
    sha256_final(&c, head);
}

int rxl_seal(rxl_log *l) {
    if (l->n && l->recs[l->n - 1].type == RXL_END) l->n--;
    uint8_t (*dig)[32] = calloc(l->n + 1, 32);
    uint64_t *ep = calloc(l->n + 1, sizeof *ep);
    if (!dig || !ep) { free(dig); free(ep); return -1; }
    uint64_t nk = 0;
    uint8_t head[32] = { 0 };
    for (size_t i = 0; i < l->n; i++) {
        rxl_rec *r = &l->recs[i];
        if (r->type == RXL_CRUMB) {
            rxl_crumb *k = &r->u.c;
            rxl_crumb_digest(k, (const uint8_t (*)[32])dig, nk, k->digest);
            k->episode = k->kind == RXL_K_EXTERNAL ? k->id
                       : k->wake_cause >= 1 && k->wake_cause < k->id && k->wake_cause <= nk
                           ? ep[k->wake_cause - 1] : 0;
            memcpy(dig[nk], k->digest, 32);
            ep[nk] = k->episode;
            nk++;
        }
        rxl_head_step(head, r);
    }
    rxl_rec e;
    memset(&e, 0, sizeof e);
    e.type = RXL_END;
    e.u.end.n_records = l->n;
    memcpy(e.u.end.head, head, 32);
    free(dig); free(ep);
    return rxl_push(l, &e);
}

void rxl_hex(const uint8_t *d, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[2 * n] = 0;
}

int rxl_finish(rxl_log *l) {
    if (l->n && l->recs[l->n - 1].type == RXL_END) l->n--;
    uint8_t head[32] = { 0 };
    for (size_t i = 0; i < l->n; i++) rxl_head_step(head, &l->recs[i]);
    rxl_rec e;
    memset(&e, 0, sizeof e);
    e.type = RXL_END;
    e.u.end.n_records = l->n;
    memcpy(e.u.end.head, head, 32);
    return rxl_push(l, &e);
}
