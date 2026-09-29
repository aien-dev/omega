/* Reference coder B: rANS with 32-bit state and byte renormalization.
 * See tc_rans.h and CODER_SPEC.md section 6. */
#include "turing/tc_rans.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

#define RANS_SCALE 16

static int row_cum(const uint16_t *q, unsigned K, unsigned s, uint32_t *cum) {
    uint32_t tot = 0, c = 0;
    for (unsigned x = 0; x < K; ++x) {
        if (!q[x]) return TC_E_ZERO;
        if (x == s) c = tot;
        tot += q[x];
    }
    if (tot != TC_QONE) return TC_E_NORM;
    *cum = c;
    return TC_OK;
}

int tc_rans_encode_raw(const uint16_t *q, unsigned K, const uint8_t *sym, uint64_t n, uint8_t **out, size_t *len) {
    if (!q || !sym || !out || !len || K < TC_KMIN || K > TC_KMAX || n > (SIZE_MAX - 8) / 2) return TC_E_ARG;
    size_t cap = (size_t)n * 2 + 4; /* at most 2 renorm bytes per symbol (f >= 1) + 4-byte flush */
    uint8_t *buf = malloc(cap);
    if (!buf) return TC_E_IO;
    size_t p = cap; /* write pointer, moves toward 0 */
    uint32_t x = TC_RANS_L;
    for (uint64_t i = n; i-- > 0;) {
        const uint16_t *row = q + i * K;
        unsigned s = sym[i];
        uint32_t c;
        int rc = s < K ? row_cum(row, K, s, &c) : TC_E_SYMBOL;
        if (rc != TC_OK) {
            free(buf);
            return rc;
        }
        uint32_t f = row[s];
        uint32_t x_max = ((TC_RANS_L >> RANS_SCALE) << 8) * f;
        while (x >= x_max) {
            buf[--p] = (uint8_t)(x & 0xFF);
            x >>= 8;
        }
        x = ((x / f) << RANS_SCALE) + (x % f) + c;
    }
    p -= 4;
    buf[p] = (uint8_t)x;
    buf[p + 1] = (uint8_t)(x >> 8);
    buf[p + 2] = (uint8_t)(x >> 16);
    buf[p + 3] = (uint8_t)(x >> 24);
    size_t nb = cap - p;
    memmove(buf, buf + p, nb);
    *out = buf;
    *len = nb;
    return TC_OK;
}

int tc_rans_decode_raw(const uint16_t *q, unsigned K, const uint8_t *in, size_t len, uint64_t n, uint8_t *sym,
                       char *why, size_t whylen) {
    if (!q || !in || !sym || K < TC_KMIN || K > TC_KMAX) return TC_E_ARG;
    if (len < 4) {
        WHY("rANS payload has %zu bytes, the decoder needs at least 4 (truncated bitstream)", len);
        return TC_E_TRUNC;
    }
    uint32_t x = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    size_t pos = 4;
    if (x < TC_RANS_L || x >= (1u << 31)) {
        WHY("rANS initial state %u outside [2^23, 2^31)", x);
        return TC_E_CORRUPT;
    }
    for (uint64_t t = 0; t < n; ++t) {
        const uint16_t *row = q + t * K;
        uint32_t m = x & (TC_QONE - 1), cum = 0, tot = 0;
        unsigned s = K;
        for (unsigned y = 0; y < K; ++y) {
            if (!row[y]) return TC_E_ZERO;
            if (s == K && m < tot + row[y]) {
                s = y;
                cum = tot;
            }
            tot += row[y];
        }
        if (tot != TC_QONE || s == K) return TC_E_NORM;
        sym[t] = (uint8_t)s;
        x = (uint32_t)row[s] * (x >> RANS_SCALE) + m - cum;
        while (x < TC_RANS_L) {
            if (pos >= len) {
                WHY("rANS payload ended at symbol %" PRIu64 " of %" PRIu64 " (truncated bitstream)", t, n);
                return TC_E_TRUNC;
            }
            x = (x << 8) | in[pos++];
        }
    }
    if (x != TC_RANS_L) {
        WHY("rANS termination check failed: final state %u, want %u", x, TC_RANS_L);
        return TC_E_CORRUPT;
    }
    if (pos != len) {
        WHY("rANS payload has %zu bytes after the last symbol (trailing bytes)", len - pos);
        return TC_E_TRAIL;
    }
    return TC_OK;
}

int tc_rans_encode(const tc_pstream *p, const tc_symbols *s, uint8_t **out, size_t *len, char *why, size_t whylen) {
    if (!p || !s || !out || !len) return TC_E_ARG;
    int rc = tc_pair_check(p, s, why, whylen);
    if (rc != TC_OK) return rc;
    if ((rc = tc_ps_check_rows(p, why, whylen)) != TC_OK) return rc;
    uint8_t *pl;
    size_t pn;
    if ((rc = tc_rans_encode_raw(p->q, p->K, s->sym, s->n, &pl, &pn)) != TC_OK) {
        WHY("rANS encode failed: %s", tc_err_name(rc));
        return rc;
    }
    uint8_t *b = malloc(TC_CODED_HEADER + pn);
    if (!b) {
        free(pl);
        return TC_E_IO;
    }
    tc_coded_header_write(b, TC_RANS_MAGIC, TC_RANS_ID, p->n, p->digest, pn);
    memcpy(b + TC_CODED_HEADER, pl, pn);
    free(pl);
    *out = b;
    *len = TC_CODED_HEADER + pn;
    return TC_OK;
}

int tc_rans_decode(const tc_pstream *p, const uint8_t *in, size_t len, tc_symbols *s, char *why, size_t whylen) {
    if (!p || !in || !s) return TC_E_ARG;
    memset(s, 0, sizeof *s);
    const uint8_t *pl;
    size_t pn;
    int rc = tc_coded_header_check(in, len, TC_RANS_MAGIC, TC_RANS_ID, p, &pl, &pn, why, whylen);
    if (rc != TC_OK) return rc;
    if ((rc = tc_sy_alloc(s, p->K, p->n)) != TC_OK) return rc;
    memcpy(s->dataset, p->dataset, TC_DIGEST);
    if ((rc = tc_rans_decode_raw(p->q, p->K, pl, pn, p->n, s->sym, why, whylen)) != TC_OK) tc_sy_free(s);
    return rc;
}
