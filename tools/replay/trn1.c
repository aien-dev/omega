/* trn1.c -- TRN1 verifier, comparator and RXCLOG01 export (see trn1.h).
 * Written from TRN1_TRANSCRIPT_SPEC.md sections 3-8; check order is the
 * spec's section 7, which fixes the refusal code. */
#include "replay/trn1.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR 48u
#define RHDR 56u
#define MAXLEN 65536u
#define T_END 0xFFFFu
#define N_SUBSYS 11u

static const char *SUBSYS[N_SUBSYS] = {
    "transcript", "omega-world", "omega-cortex", "omega-jspace", "aienos-kernel", "aienos-argus",
    "aienos-store", "sovcore-runtime", "sovcore-scheduler", "sovcore-kv", "external" };

const char *trn1_subsystem_name(uint16_t id) { return id < N_SUBSYS ? SUBSYS[id] : "?"; }

static uint16_t l16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t l32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t l64(const uint8_t *p) { return (uint64_t)l32(p) | (uint64_t)l32(p + 4) << 32; }
static int zero(const uint8_t *p, size_t n) { while (n--) if (*p++) return 0; return 1; }

static int known_type(uint16_t t) { return (t >= 1 && t <= 16) || t == T_END; }

/* Step 8 part 1: identity length rule. */
static int ident_len_ok(uint16_t t, uint32_t n) {
    static const uint32_t exact[17] = { 0, 32, 24, 24, 40, 24, 48, 32, 56, 56, 48, 0, 16, 48, 40, 0, 200 };
    if (t == T_END) return n == 16;
    if (t == 11) return n >= 48 && n <= 48 + 4096;
    if (t == 15) return n >= 32;
    return n == exact[t];
}

/* RX_CRUMB canonical bytes must parse to exactly their length. */
static int crumb_parse(const uint8_t *p, size_t n) {
    size_t o = 0;
#define NEED(k) do { if (n - o < (size_t)(k)) return 0; } while (0)
    NEED(8 + 4 + 4 + 4 + 8 + 8);
    uint64_t id = l64(p);
    o = 36;
    NEED(4);
    uint32_t c = l32(p + o); o += 4;
    if (c > 8) return 0;
    NEED(24ull * c); o += 24ull * c;
    NEED(4);
    c = l32(p + o); o += 4;
    if (c > 8) return 0;
    NEED(16ull * c); o += 16ull * c;
    NEED(4);
    c = l32(p + o); o += 4;
    if (c > 8) return 0;
    NEED(24ull * c); o += 24ull * c;
    NEED(4 + 4);
    o += 4;
    c = l32(p + o); o += 4;
    if (c > 65) return 0;
    for (uint32_t i = 0; i < c; i++) {
        NEED(8);
        uint64_t par = l64(p + o); o += 8;
        if (par >= 1 && par < id) { NEED(32); o += 32; }
    }
#undef NEED
    return o == n;
}

/* Step 9: reserved fields inside the identity. */
static int ident_reserved_zero(uint16_t t, const uint8_t *id) {
    switch (t) {
    case 3: return zero(id + 20, 4);
    case 6: return zero(id + 4, 4);
    case 7: return zero(id + 5, 3);
    case 10: return zero(id + 12, 4);
    case 11: return zero(id + 4, 4);
    case 12: return zero(id + 4, 4);
    case 14: return zero(id + 4, 4);
    case 16: return zero(id + 129, 7);
    case T_END: return zero(id + 8, 8);
    default: return 1;
    }
}

static void cmp_digest(const uint8_t *rec, uint32_t ilen, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_TRN1_CMP", 13);
    sha256_update(&c, rec, 4);            /* type, subsystem */
    sha256_update(&c, rec + 16, 8);       /* seq */
    sha256_update(&c, rec + 4, 4);        /* ident_len */
    sha256_update(&c, rec + RHDR, ilen);
    sha256_final(&c, out);
}

static int refuse(trn1_result *r, int code, uint64_t ev) { r->code = code; r->event = ev; return code; }

int trn1_verify(const uint8_t *b, size_t len, trn1_result *r) {
    memset(r, 0, sizeof *r);
    if (len < 4) return refuse(r, TRN1_LENGTH, 0);
    if (memcmp(b, "TRN1", 4)) return refuse(r, TRN1_MAGIC, 0);
    if (len < 6) return refuse(r, TRN1_LENGTH, 0);
    if (l16(b + 4) != 1) return refuse(r, TRN1_VERSION, 0);
    if (len < HDR) return refuse(r, TRN1_LENGTH, 0);
    if (l16(b + 6) != 0) return refuse(r, TRN1_NONCANONICAL, 0);
    uint16_t prod = l16(b + 40);
    if (prod == 0 || prod >= N_SUBSYS) return refuse(r, TRN1_UNKNOWN, 0);
    if (!zero(b + 42, 6)) return refuse(r, TRN1_NONCANONICAL, 0);
    memcpy(r->run_id, b + 8, 32);
    uint8_t prev[32];
    sha256_hash(b, HDR, prev);
    r->cmp = calloc(TRN1_MAX_RECORDS, 32);
    r->subsys = calloc(TRN1_MAX_RECORDS, sizeof *r->subsys);
    if (!r->cmp || !r->subsys) { trn1_result_free(r); return refuse(r, TRN1_LENGTH, 0); }
    int have_argus = 0;
    uint8_t argus_after[32];
    size_t pos = HDR;
    for (uint64_t k = 1;; k++) {
        if (pos == len) return refuse(r, TRN1_TRUNCATED, k);
        if (len - pos < RHDR || k > TRN1_MAX_RECORDS) return refuse(r, TRN1_LENGTH, k);
        const uint8_t *h = b + pos;
        uint16_t type = l16(h), sub = l16(h + 2);
        uint32_t ilen = l32(h + 4), alen = l32(h + 8);
        if (!known_type(type) || sub >= N_SUBSYS) return refuse(r, TRN1_UNKNOWN, k);
        if (!zero(h + 12, 4)) return refuse(r, TRN1_NONCANONICAL, k);
        if (ilen > MAXLEN || alen > MAXLEN) return refuse(r, TRN1_LENGTH, k);
        if (l64(h + 16) != k) return refuse(r, TRN1_GAP, k);
        if (memcmp(h + 24, prev, 32)) return refuse(r, TRN1_CHAIN, k);
        if ((uint64_t)len - pos - RHDR < (uint64_t)ilen + alen) return refuse(r, TRN1_LENGTH, k);
        const uint8_t *id = h + RHDR;
        int shape = ident_len_ok(type, ilen) && ((type == T_END) == (sub == 0));
        if (shape && type == 7) shape = id[4] >= 1 && id[4] <= 6;
        if (shape && type == 13) shape = l64(id + 8) < k;
        if (shape && type == 15) shape = crumb_parse(id + 32, ilen - 32);
        if (shape && type == 16) shape = id[128] <= 1;
        if (shape && type == T_END) shape = alen == 0;
        if (!shape) return refuse(r, TRN1_SHAPE, k);
        if (!ident_reserved_zero(type, id)) return refuse(r, TRN1_NONCANONICAL, k);
        if (type == 15) {
            uint8_t d[32];
            sha256_ctx c;
            sha256_init(&c);
            sha256_update(&c, (const uint8_t *)"AIEN_RX_CAUSAL_V1", 17);
            sha256_update(&c, id + 32, ilen - 32);
            sha256_final(&c, d);
            if (memcmp(d, id, 32)) return refuse(r, TRN1_DIGEST, k);
        } else if (type == 11 && ilen > 48) {
            uint8_t d[32];
            sha256_hash(id + 48, ilen - 48, d);
            if (memcmp(d, id + 16, 32)) return refuse(r, TRN1_DIGEST, k);
        } else if (type == 16) {
            if (have_argus && memcmp(id + 136, argus_after, 32)) return refuse(r, TRN1_DIGEST, k);
            uint8_t d[32];
            if (id[128]) {
                sha256_ctx c;
                sha256_init(&c);
                sha256_update(&c, id + 136, 32);
                sha256_update(&c, id, 128);
                sha256_final(&c, d);
            } else memcpy(d, id + 136, 32);
            if (memcmp(d, id + 168, 32)) return refuse(r, TRN1_DIGEST, k);
            memcpy(argus_after, id + 168, 32);
            have_argus = 1;
        }
        cmp_digest(h, ilen, r->cmp[k - 1]);
        r->subsys[k - 1] = sub;
        sha256_hash(h, RHDR + (size_t)ilen + alen, prev);
        pos += RHDR + (size_t)ilen + alen;
        if (type == T_END) {
            if (l64(id) != k) return refuse(r, TRN1_GAP, k);
            if (pos != len) return refuse(r, TRN1_LENGTH, k + 1);
            r->records = k;
            memcpy(r->final, prev, 32);
            return TRN1_OK;
        }
    }
}

void trn1_result_free(trn1_result *r) {
    free(r->cmp); free(r->subsys);
    r->cmp = NULL; r->subsys = NULL;
}

void trn1_verify_line(const trn1_result *r, char *out, size_t n) {
    if (r->code) { snprintf(out, n, "refuse %d event=%llu", r->code, (unsigned long long)r->event); return; }
    char a[65], b[65];
    rxl_hex(r->run_id, 32, a);
    rxl_hex(r->final, 32, b);
    snprintf(out, n, "ok records=%llu run=%s final=%s", (unsigned long long)r->records, a, b);
}

void trn1_compare_line(const uint8_t *a, size_t na, const uint8_t *b, size_t nb, char *out, size_t n) {
    trn1_result x, y;
    if (trn1_verify(a, na, &x)) {
        snprintf(out, n, "refuse expected %d event=%llu", x.code, (unsigned long long)x.event);
        trn1_result_free(&x);
        return;
    }
    if (trn1_verify(b, nb, &y)) {
        snprintf(out, n, "refuse actual %d event=%llu", y.code, (unsigned long long)y.event);
        trn1_result_free(&x); trn1_result_free(&y);
        return;
    }
    uint64_t m = x.records < y.records ? x.records : y.records;
    snprintf(out, n, "MATCH through %llu", (unsigned long long)x.records);
    for (uint64_t k = 0; k < m; k++) {
        if (memcmp(x.cmp[k], y.cmp[k], 32)) {
            char ha[65], hb[65];
            rxl_hex(x.cmp[k], 32, ha);
            rxl_hex(y.cmp[k], 32, hb);
            snprintf(out, n, "DIVERGENCE event=%llu expected=%s actual=%s subsystem=%s", (unsigned long long)k + 1,
                     ha, hb, trn1_subsystem_name(x.subsys[k]));
            break;
        }
    }
    trn1_result_free(&x); trn1_result_free(&y);
}

/* ---- RXCLOG01 -> TRN1 ---------------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; int bad; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->n + n) nc *= 2;
        uint8_t *q = realloc(b->p, nc);
        if (!q) { b->bad = 1; return; }
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void p16(buf *b, uint16_t v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(b, x, 2); }
static void p32(buf *b, uint32_t v) { uint8_t x[4]; for (int i = 0; i < 4; i++) x[i] = (uint8_t)(v >> (8 * i)); put(b, x, 4); }
static void p64(buf *b, uint64_t v) { p32(b, (uint32_t)v); p32(b, (uint32_t)(v >> 32)); }
static void pio(buf *b, const rxl_io *io) { p32(b, io->id); p32(b, io->gen); p64(b, io->version); p64(b, io->mask); }

static int emit(buf *out, uint8_t prev[32], uint64_t seq, uint16_t type, uint16_t sub,
                const buf *ident, const buf *annot) {
    size_t start = out->n;
    p16(out, type); p16(out, sub);
    p32(out, (uint32_t)ident->n); p32(out, (uint32_t)annot->n); p32(out, 0);
    p64(out, seq);
    put(out, prev, 32);
    put(out, ident->p, ident->n);
    put(out, annot->p, annot->n);
    if (out->bad) return -1;
    sha256_hash(out->p + start, out->n - start, prev);
    return 0;
}

int trn1_from_rxlog(const rxl_log *l, uint8_t **out_p, size_t *out_len) {
    buf o = { 0 }, id = { 0 }, an = { 0 };
    uint8_t (*dig)[32] = calloc(l->n + 1, 32);
    if (!dig) return -1;
    uint8_t run[32] = { 0 };
    if (l->n && l->recs[l->n - 1].type == RXL_END) memcpy(run, l->recs[l->n - 1].u.end.head, 32);
    put(&o, "TRN1", 4);
    p16(&o, 1); p16(&o, 0);
    put(&o, run, 32);
    p16(&o, 1);                      /* producer omega-world */
    uint8_t z[32] = { 0 };
    put(&o, z, 6);
    uint8_t prev[32];
    if (o.bad) { free(dig); return -1; }
    sha256_hash(o.p, 48, prev);
    uint64_t seq = 0, nk = 0, nin = 0;
    int rc = 0;
    for (size_t i = 0; i < l->n && !rc; i++) {
        const rxl_rec *r = &l->recs[i];
        if (r->type == RXL_END) break;
        id.n = an.n = 0;
        if (r->type == RXL_CRUMB) {
            const rxl_crumb *k = &r->u.c;
            put(&id, k->digest, 32);
            p64(&id, k->id); p32(&id, k->kind); p32(&id, k->reaction); p32(&id, k->faculty);
            p64(&id, k->wake_cause); p64(&id, k->coalesced);
            p32(&id, k->n_inputs);
            for (uint32_t j = 0; j < k->n_inputs; j++) pio(&id, &k->inputs[j]);
            p32(&id, k->n_caps);
            for (uint32_t j = 0; j < k->n_caps; j++) { p32(&id, k->caps[j].cap_id); p64(&id, k->caps[j].gen); p32(&id, k->caps[j].issuer); }
            p32(&id, k->n_outputs);
            for (uint32_t j = 0; j < k->n_outputs; j++) pio(&id, &k->outputs[j]);
            p32(&id, (uint32_t)k->reason);
            p32(&id, k->n_parents);
            for (uint32_t j = 0; j < k->n_parents; j++) {
                uint64_t p = k->parents[j];
                p64(&id, p);
                if (p >= 1 && p < k->id) put(&id, p <= nk ? dig[p - 1] : z, 32);
            }
            p32(&an, k->worker); p64(&an, k->t_start); p64(&an, k->t_end); p64(&an, k->episode);
            if (nk < l->n) memcpy(dig[nk++], k->digest, 32);
            rc = emit(&o, prev, ++seq, 15, 1, &id, &an);
        } else if (r->type == RXL_INPUT) {
            uint8_t content[RXL_MAX_PAYLOAD], d[32];
            size_t cn = rxl_encode(r, content);
            sha256_hash(content, cn, d);
            p32(&id, 1); p32(&id, 0); p64(&id, ++nin);
            put(&id, d, 32);
            put(&id, content, cn);
            rc = emit(&o, prev, ++seq, 11, 10, &id, &an);
        } else if (r->type == RXL_CHECKPOINT) {
            p32(&id, r->u.ck.subsystem); p32(&id, 0);
            put(&id, r->u.ck.hash, 32);
            rc = emit(&o, prev, ++seq, 14, 1, &id, &an);
        }
    }
    if (!rc) {
        id.n = an.n = 0;
        p64(&id, seq + 1); p64(&id, 0);
        rc = emit(&o, prev, seq + 1, (uint16_t)T_END, 0, &id, &an);
    }
    free(dig); free(id.p); free(an.p);
    if (rc || o.bad) { free(o.p); return -1; }
    *out_p = o.p;
    *out_len = o.n;
    return 0;
}
