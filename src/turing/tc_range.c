/* Reference coder A: range coder. See tc_range.h and CODER_SPEC.md section 5. */
#include "turing/tc_range.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

#define RC_TOP (1u << 24)

typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    int have_cache;   /* 0 until the first byte has been recorded */
    uint64_t pending; /* deferred 0xFF bytes after the cache */
    uint8_t *out;
    size_t n, cap;
    int err;
    tc_range_stats st;
} renc;

static void emit(renc *e, uint8_t b) {
    if (e->n == e->cap) {
        size_t nc = e->cap ? e->cap * 2 : 4096;
        uint8_t *p = realloc(e->out, nc);
        if (!p) {
            e->err = 1;
            return;
        }
        e->out = p;
        e->cap = nc;
    }
    e->out[e->n++] = b;
}

/* Move the top byte of the 32-bit window out, resolving any carry (bit 32 of low). */
static void shift_low(renc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        if (carry) {
            e->st.carries++;
            if (!e->have_cache) e->err = 2; /* carry past the first byte: impossible, refuse */
        }
        if (e->have_cache) emit(e, (uint8_t)(e->cache + carry));
        for (; e->pending; --e->pending) emit(e, (uint8_t)(0xFFu + carry));
        e->cache = (uint8_t)(e->low >> 24);
        e->have_cache = 1;
    } else {
        e->pending++;
        if (e->pending > e->st.pending_max) e->st.pending_max = e->pending;
    }
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

static int row_ok(const uint16_t *q, unsigned K) {
    uint32_t s = 0;
    for (unsigned x = 0; x < K; ++x) {
        if (!q[x]) return 0;
        s += q[x];
    }
    return s == TC_QONE;
}

int tc_range_encode_raw(const uint16_t *q, unsigned K, const uint8_t *sym, uint64_t n, uint8_t **out, size_t *len,
                        tc_range_stats *st) {
    if (!q || !sym || !out || !len || K < TC_KMIN || K > TC_KMAX) return TC_E_ARG;
    renc e;
    memset(&e, 0, sizeof e);
    e.range = 0xFFFFFFFFu;
    for (uint64_t t = 0; t < n; ++t) {
        const uint16_t *row = q + t * K;
        unsigned s = sym[t];
        if (s >= K) {
            free(e.out);
            return TC_E_SYMBOL;
        }
        if (!row_ok(row, K)) {
            free(e.out);
            return TC_E_NORM;
        }
        uint32_t cum = 0;
        for (unsigned x = 0; x < s; ++x) cum += row[x];
        uint32_t r = e.range >> 16;
        e.low += (uint64_t)r * cum;
        e.range = (s == K - 1) ? e.range - r * cum : r * row[s];
        while (e.range < RC_TOP) {
            e.range <<= 8;
            shift_low(&e);
        }
    }
    for (int i = 0; i < 5; ++i) shift_low(&e);
    if (e.err) {
        free(e.out);
        return e.err == 2 ? TC_E_CORRUPT : TC_E_IO;
    }
    if (!e.out) {
        e.out = malloc(1);
        if (!e.out) return TC_E_IO;
    }
    *out = e.out;
    *len = e.n;
    if (st) *st = e.st;
    return TC_OK;
}

int tc_range_decode_raw(const uint16_t *q, unsigned K, const uint8_t *in, size_t len, uint64_t n, uint8_t *sym,
                        char *why, size_t whylen) {
    if (!q || !in || !sym || K < TC_KMIN || K > TC_KMAX) return TC_E_ARG;
    size_t pos = 0;
    if (len < 4) {
        WHY("range payload has %zu bytes, the decoder needs at least 4 (truncated bitstream)", len);
        return TC_E_TRUNC;
    }
    uint32_t code = 0, range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i) code = (code << 8) | in[pos++];
    for (uint64_t t = 0; t < n; ++t) {
        if (code >= range) {
            WHY("range decoder state invalid at symbol %" PRIu64 " (code >= range)", t);
            return TC_E_CORRUPT;
        }
        const uint16_t *row = q + t * K;
        if (!row_ok(row, K)) return TC_E_NORM;
        uint32_t r = range >> 16, cum = 0;
        unsigned s = 0;
        for (; s + 1 < K; ++s) {
            uint32_t next = cum + row[s];
            if (code < r * next) break;
            cum = next;
        }
        sym[t] = (uint8_t)s;
        code -= r * cum;
        range = (s == K - 1) ? range - r * cum : r * row[s];
        while (range < RC_TOP) {
            if (pos >= len) {
                WHY("range payload ended at symbol %" PRIu64 " of %" PRIu64 " (truncated bitstream)", t, n);
                return TC_E_TRUNC;
            }
            code = (code << 8) | in[pos++];
            range <<= 8;
        }
    }
    if (code != 0) {
        WHY("range termination check failed: final code %u, want 0", code);
        return TC_E_CORRUPT;
    }
    if (pos != len) {
        WHY("range payload has %zu bytes after the last symbol (trailing bytes)", len - pos);
        return TC_E_TRAIL;
    }
    return TC_OK;
}

int tc_range_encode(const tc_pstream *p, const tc_symbols *s, uint8_t **out, size_t *len, char *why, size_t whylen) {
    if (!p || !s || !out || !len) return TC_E_ARG;
    int rc = tc_pair_check(p, s, why, whylen);
    if (rc != TC_OK) return rc;
    if ((rc = tc_ps_check_rows(p, why, whylen)) != TC_OK) return rc;
    uint8_t *pl;
    size_t pn;
    if ((rc = tc_range_encode_raw(p->q, p->K, s->sym, s->n, &pl, &pn, NULL)) != TC_OK) {
        WHY("range encode failed: %s", tc_err_name(rc));
        return rc;
    }
    uint8_t *b = malloc(TC_CODED_HEADER + pn);
    if (!b) {
        free(pl);
        return TC_E_IO;
    }
    tc_coded_header_write(b, TC_RANGE_MAGIC, TC_RANGE_ID, p->n, p->digest, pn);
    memcpy(b + TC_CODED_HEADER, pl, pn);
    free(pl);
    *out = b;
    *len = TC_CODED_HEADER + pn;
    return TC_OK;
}

int tc_range_decode(const tc_pstream *p, const uint8_t *in, size_t len, tc_symbols *s, char *why, size_t whylen) {
    if (!p || !in || !s) return TC_E_ARG;
    memset(s, 0, sizeof *s);
    const uint8_t *pl;
    size_t pn;
    int rc = tc_coded_header_check(in, len, TC_RANGE_MAGIC, TC_RANGE_ID, p, &pl, &pn, why, whylen);
    if (rc != TC_OK) return rc;
    if ((rc = tc_sy_alloc(s, p->K, p->n)) != TC_OK) return rc;
    memcpy(s->dataset, p->dataset, TC_DIGEST);
    if ((rc = tc_range_decode_raw(p->q, p->K, pl, pn, p->n, s->sym, why, whylen)) != TC_OK) tc_sy_free(s);
    return rc;
}
