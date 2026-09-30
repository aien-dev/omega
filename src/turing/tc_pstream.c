/* TPS1 / TSY1 reader and writer. See tc_pstream.h and calibration/docs/CODER_SPEC.md. */
#include "turing/tc_pstream.h"

#include "sha256.h"
#include "turing/ty_math.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

const char *tc_err_name(int e) {
    switch (e) {
    case TC_OK: return "OK";
    case TC_E_ARG: return "TC_E_ARG";
    case TC_E_IO: return "TC_E_IO";
    case TC_E_FORMAT: return "TC_E_FORMAT";
    case TC_E_DIGEST: return "TC_E_DIGEST";
    case TC_E_NORM: return "TC_E_NORM";
    case TC_E_ZERO: return "TC_E_ZERO";
    case TC_E_PROFILE: return "TC_E_PROFILE";
    case TC_E_MODEL: return "TC_E_MODEL";
    case TC_E_DATASET: return "TC_E_DATASET";
    case TC_E_COUNT: return "TC_E_COUNT";
    case TC_E_HEADER: return "TC_E_HEADER";
    case TC_E_BINDING: return "TC_E_BINDING";
    case TC_E_TRUNC: return "TC_E_TRUNC";
    case TC_E_TRAIL: return "TC_E_TRAIL";
    case TC_E_CORRUPT: return "TC_E_CORRUPT";
    case TC_E_SYMBOL: return "TC_E_SYMBOL";
    case TC_E_INDEX: return "TC_E_INDEX";
    default: return "TC_E_UNKNOWN";
    }
}

void tc_put_u16(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}
void tc_put_u32(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(v >> (8 * i));
}
void tc_put_u64(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i));
}
uint16_t tc_get_u16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
uint32_t tc_get_u32(const uint8_t *b) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}
uint64_t tc_get_u64(const uint8_t *b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

void tc_hex(const uint8_t d[TC_DIGEST], char out[65]) {
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < TC_DIGEST; ++i) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[64] = 0;
}

int tc_unhex(const char *hex, uint8_t d[TC_DIGEST]) {
    if (!hex || strlen(hex) != 64) return TC_E_ARG;
    for (int i = 0; i < 64; ++i) {
        char c = hex[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (v < 0) return TC_E_ARG;
        if (i & 1)
            d[i / 2] = (uint8_t)(d[i / 2] | v);
        else
            d[i / 2] = (uint8_t)(v << 4);
    }
    return TC_OK;
}

static void dom_digest(const char *dom, const uint8_t *buf, size_t len, uint8_t out[TC_DIGEST]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)dom, strlen(dom) + 1); /* includes 0x00 */
    sha256_update(&c, buf, len);
    sha256_final(&c, out);
}

void tc_ps_free(tc_pstream *p) {
    if (!p) return;
    free(p->key);
    free(p->crumb);
    free(p->q);
    memset(p, 0, sizeof *p);
}

void tc_sy_free(tc_symbols *s) {
    if (!s) return;
    free(s->sym);
    memset(s, 0, sizeof *s);
}

int tc_ps_alloc(tc_pstream *p, unsigned K, uint64_t n) {
    if (!p || K < TC_KMIN || K > TC_KMAX || n > (SIZE_MAX / 64)) return TC_E_ARG;
    memset(p, 0, sizeof *p);
    p->K = K;
    p->n = n;
    size_t m = n ? (size_t)n : 1;
    p->key = calloc(m, sizeof *p->key);
    p->crumb = calloc(m, sizeof *p->crumb);
    p->q = calloc(m * K, sizeof *p->q);
    if (!p->key || !p->crumb || !p->q) {
        tc_ps_free(p);
        return TC_E_IO;
    }
    return TC_OK;
}

int tc_sy_alloc(tc_symbols *s, unsigned K, uint64_t n) {
    if (!s || K < TC_KMIN || K > TC_KMAX || n > SIZE_MAX / 2) return TC_E_ARG;
    memset(s, 0, sizeof *s);
    s->K = K;
    s->n = n;
    s->sym = calloc(n ? (size_t)n : 1, 1);
    return s->sym ? TC_OK : TC_E_IO;
}

static int check_row(const uint16_t *q, unsigned K, uint32_t norm_sum, uint64_t t, char *why, size_t whylen) {
    uint32_t s = 0;
    for (unsigned x = 0; x < K; ++x) {
        if (q[x] == 0) {
            WHY("record %" PRIu64 ": probability entry %u is 0 (zero-probability policy: refuse)", t, x);
            return TC_E_ZERO;
        }
        s += q[x];
    }
    if (s != TC_QONE || norm_sum != TC_QONE) {
        WHY("record %" PRIu64 ": entries sum to %u, norm_sum field %u (both must be 65536)", t, s, norm_sum);
        return TC_E_NORM;
    }
    return TC_OK;
}

int tc_ps_check_rows(const tc_pstream *p, char *why, size_t whylen) {
    if (!p || p->K < TC_KMIN || p->K > TC_KMAX) return TC_E_ARG;
    for (uint64_t t = 0; t < p->n; ++t) {
        int rc = check_row(p->q + t * p->K, p->K, TC_QONE, t, why, whylen);
        if (rc != TC_OK) return rc;
    }
    return TC_OK;
}

int tc_ps_serialize(tc_pstream *p, uint8_t **buf, size_t *len, char *why, size_t whylen) {
    if (!p || !buf || !len || p->K < TC_KMIN || p->K > TC_KMAX) return TC_E_ARG;
    int rc = tc_ps_check_rows(p, why, whylen);
    if (rc != TC_OK) return rc;
    size_t rb = TC_PS_REC_FIXED + 2 * (size_t)p->K;
    size_t body = TC_PS_HEADER + (size_t)p->n * rb;
    uint8_t *b = malloc(body + TC_DIGEST);
    if (!b) return TC_E_IO;
    memset(b, 0, TC_PS_HEADER);
    memcpy(b, "TPS1", 4);
    tc_put_u16(b + 4, TC_PS_VERSION);
    b[6] = (uint8_t)p->K;
    b[7] = 16;
    tc_put_u32(b + 8, 0);
    tc_put_u64(b + 12, p->n);
    tc_put_u32(b + 20, (uint32_t)rb);
    tc_put_u32(b + 24, 0);
    memcpy(b + 28, p->profile, TC_DIGEST);
    memcpy(b + 60, p->model, TC_DIGEST);
    memcpy(b + 92, p->dataset, TC_DIGEST);
    /* bytes 124..127 reserved zero */
    uint8_t *r = b + TC_PS_HEADER;
    for (uint64_t t = 0; t < p->n; ++t, r += rb) {
        tc_put_u64(r, t);
        tc_put_u64(r + 8, p->key[t]);
        tc_put_u32(r + 16, p->crumb[t]);
        tc_put_u32(r + 20, TC_QONE);
        const uint16_t *q = p->q + t * p->K;
        for (unsigned x = 0; x < p->K; ++x) tc_put_u16(r + 24 + 2 * x, q[x]);
    }
    dom_digest(TC_PS_DOMAIN, b, body, b + body);
    memcpy(p->digest, b + body, TC_DIGEST);
    *buf = b;
    *len = body + TC_DIGEST;
    return TC_OK;
}

int tc_ps_parse(const uint8_t *buf, size_t len, tc_pstream *p, char *why, size_t whylen) {
    if (!buf || !p) return TC_E_ARG;
    memset(p, 0, sizeof *p);
    if (len < TC_PS_HEADER + TC_DIGEST || memcmp(buf, "TPS1", 4) != 0) {
        WHY("not a TPS1 probability stream (magic or size)");
        return TC_E_FORMAT;
    }
    uint8_t d[TC_DIGEST];
    dom_digest(TC_PS_DOMAIN, buf, len - TC_DIGEST, d);
    if (memcmp(d, buf + len - TC_DIGEST, TC_DIGEST) != 0) {
        WHY("TPS1 trailer digest does not match the stream bytes (corrupted probability stream)");
        return TC_E_DIGEST;
    }
    unsigned ver = tc_get_u16(buf + 4), K = buf[6], qb = buf[7];
    uint32_t flags = tc_get_u32(buf + 8), rbw = tc_get_u32(buf + 20), res = tc_get_u32(buf + 24);
    uint64_t n = tc_get_u64(buf + 12);
    int reszero = 1;
    for (int i = 124; i < TC_PS_HEADER; ++i) reszero &= buf[i] == 0;
    if (ver != TC_PS_VERSION || qb != 16 || flags || res || !reszero || K < TC_KMIN || K > TC_KMAX) {
        WHY("TPS1 header: version %u qbits %u K %u flags %u reserved %u (want 1, 16, 2..16, 0, 0)", ver, qb, K,
            flags, res);
        return TC_E_FORMAT;
    }
    size_t rb = TC_PS_REC_FIXED + 2 * (size_t)K;
    if (rbw != rb || n > (len - TC_PS_HEADER - TC_DIGEST) / rb || TC_PS_HEADER + n * rb + TC_DIGEST != len) {
        WHY("TPS1 size %zu disagrees with count %" PRIu64 " x record %u bytes", len, n, rbw);
        return TC_E_FORMAT;
    }
    int rc = tc_ps_alloc(p, K, n);
    if (rc != TC_OK) return rc;
    memcpy(p->profile, buf + 28, TC_DIGEST);
    memcpy(p->model, buf + 60, TC_DIGEST);
    memcpy(p->dataset, buf + 92, TC_DIGEST);
    memcpy(p->digest, d, TC_DIGEST);
    const uint8_t *r = buf + TC_PS_HEADER;
    for (uint64_t t = 0; t < n; ++t, r += rb) {
        if (tc_get_u64(r) != t) {
            WHY("record %" PRIu64 ": observation_index %" PRIu64 " is not its position", t, tc_get_u64(r));
            tc_ps_free(p);
            return TC_E_INDEX;
        }
        p->key[t] = tc_get_u64(r + 8);
        p->crumb[t] = tc_get_u32(r + 16);
        uint16_t *q = p->q + t * K;
        for (unsigned x = 0; x < K; ++x) q[x] = tc_get_u16(r + 24 + 2 * x);
        if ((rc = check_row(q, K, tc_get_u32(r + 20), t, why, whylen)) != TC_OK) {
            tc_ps_free(p);
            return rc;
        }
    }
    return TC_OK;
}

int tc_sy_serialize(tc_symbols *s, uint8_t **buf, size_t *len, char *why, size_t whylen) {
    if (!s || !buf || !len || s->K < TC_KMIN || s->K > TC_KMAX) return TC_E_ARG;
    for (uint64_t t = 0; t < s->n; ++t)
        if (s->sym[t] >= s->K) {
            WHY("symbol %" PRIu64 " = %u outside 0..%u", t, s->sym[t], s->K - 1);
            return TC_E_SYMBOL;
        }
    size_t body = TC_SY_HEADER + (size_t)s->n;
    uint8_t *b = malloc(body + TC_DIGEST);
    if (!b) return TC_E_IO;
    memset(b, 0, TC_SY_HEADER);
    memcpy(b, "TSY1", 4);
    tc_put_u16(b + 4, TC_SY_VERSION);
    b[6] = (uint8_t)s->K;
    tc_put_u64(b + 8, s->n);
    memcpy(b + 24, s->dataset, TC_DIGEST);
    if (s->n) memcpy(b + TC_SY_HEADER, s->sym, (size_t)s->n);
    dom_digest(TC_SY_DOMAIN, b, body, b + body);
    memcpy(s->digest, b + body, TC_DIGEST);
    *buf = b;
    *len = body + TC_DIGEST;
    return TC_OK;
}

int tc_sy_parse(const uint8_t *buf, size_t len, tc_symbols *s, char *why, size_t whylen) {
    if (!buf || !s) return TC_E_ARG;
    memset(s, 0, sizeof *s);
    if (len < TC_SY_HEADER + TC_DIGEST || memcmp(buf, "TSY1", 4) != 0) {
        WHY("not a TSY1 symbol file (magic or size)");
        return TC_E_FORMAT;
    }
    uint8_t d[TC_DIGEST];
    dom_digest(TC_SY_DOMAIN, buf, len - TC_DIGEST, d);
    if (memcmp(d, buf + len - TC_DIGEST, TC_DIGEST) != 0) {
        WHY("TSY1 trailer digest does not match the symbol bytes");
        return TC_E_DIGEST;
    }
    unsigned ver = tc_get_u16(buf + 4), K = buf[6];
    uint64_t n = tc_get_u64(buf + 8);
    if (ver != TC_SY_VERSION || buf[7] || tc_get_u64(buf + 16) || K < TC_KMIN || K > TC_KMAX) {
        WHY("TSY1 header: version %u K %u or reserved nonzero", ver, K);
        return TC_E_FORMAT;
    }
    if (n != len - TC_SY_HEADER - TC_DIGEST) {
        WHY("TSY1 count %" PRIu64 " disagrees with size %zu", n, len);
        return TC_E_COUNT;
    }
    int rc = tc_sy_alloc(s, K, n);
    if (rc != TC_OK) return rc;
    memcpy(s->dataset, buf + 24, TC_DIGEST);
    memcpy(s->digest, d, TC_DIGEST);
    if (n) memcpy(s->sym, buf + TC_SY_HEADER, (size_t)n);
    for (uint64_t t = 0; t < n; ++t)
        if (s->sym[t] >= K) {
            WHY("symbol %" PRIu64 " = %u outside 0..%u", t, s->sym[t], K - 1);
            tc_sy_free(s);
            return TC_E_SYMBOL;
        }
    return TC_OK;
}

int tc_ps_expect(const tc_pstream *p, const uint8_t *profile, const uint8_t *model, const uint8_t *dataset,
                 char *why, size_t whylen) {
    if (!p) return TC_E_ARG;
    char a[65], b[65];
    if (profile && memcmp(profile, p->profile, TC_DIGEST) != 0) {
        tc_hex(p->profile, a), tc_hex(profile, b);
        WHY("profile digest %s, expected %s", a, b);
        return TC_E_PROFILE;
    }
    if (model && memcmp(model, p->model, TC_DIGEST) != 0) {
        tc_hex(p->model, a), tc_hex(model, b);
        WHY("model digest %s, expected %s", a, b);
        return TC_E_MODEL;
    }
    if (dataset && memcmp(dataset, p->dataset, TC_DIGEST) != 0) {
        tc_hex(p->dataset, a), tc_hex(dataset, b);
        WHY("dataset digest %s, expected %s", a, b);
        return TC_E_DATASET;
    }
    return TC_OK;
}

int tc_pair_check(const tc_pstream *p, const tc_symbols *s, char *why, size_t whylen) {
    if (!p || !s) return TC_E_ARG;
    if (p->K != s->K || p->n != s->n) {
        WHY("TPS1 has K %u, %" PRIu64 " observations; TSY1 has K %u, %" PRIu64 " symbols", p->K, p->n, s->K, s->n);
        return TC_E_COUNT;
    }
    if (memcmp(p->dataset, s->dataset, TC_DIGEST) != 0) {
        WHY("TPS1 and TSY1 name different dataset digests");
        return TC_E_DATASET;
    }
    for (uint64_t t = 0; t < s->n; ++t)
        if (s->sym[t] >= s->K) {
            WHY("symbol %" PRIu64 " outside alphabet", t);
            return TC_E_SYMBOL;
        }
    return TC_OK;
}

int tc_ideal_ub(const tc_pstream *p, const tc_symbols *s, uint64_t lo, uint64_t hi, int64_t *ub) {
    if (!p || !s || !ub || hi > p->n || hi > s->n || lo > hi) return TC_E_ARG;
    int64_t tot = 0;
    for (uint64_t t = lo; t < hi; ++t) {
        if (s->sym[t] >= p->K) return TC_E_SYMBOL;
        int64_t b = ty_ubits_q16(p->q[t * p->K + s->sym[t]]);
        if (b < 0) return TC_E_ZERO;
        if (ty_add(tot, b, &tot) != TY_OK) return TC_E_ARG;
    }
    *ub = tot;
    return TC_OK;
}

int tc_read_file(const char *path, uint8_t **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return TC_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return TC_E_IO;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return TC_E_IO;
    }
    uint8_t *b = malloc(sz ? (size_t)sz : 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return TC_E_IO;
    }
    fclose(f);
    *buf = b;
    *len = (size_t)sz;
    return TC_OK;
}

int tc_write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return TC_E_IO;
    size_t w = len ? fwrite(buf, 1, len, f) : 0;
    if (fclose(f) != 0 || w != len) return TC_E_IO;
    return TC_OK;
}

int tc_coded_header_write(uint8_t h[TC_CODED_HEADER], const char magic[4], unsigned coder_id, uint64_t count,
                          const uint8_t tps[TC_DIGEST], uint64_t payload) {
    memset(h, 0, TC_CODED_HEADER);
    memcpy(h, magic, 4);
    h[4] = TC_CODED_VERSION;
    h[5] = (uint8_t)coder_id;
    tc_put_u64(h + 8, count);
    memcpy(h + 16, tps, TC_DIGEST);
    tc_put_u64(h + 48, payload);
    return TC_OK;
}

int tc_coded_header_check(const uint8_t *buf, size_t len, const char magic[4], unsigned coder_id,
                          const tc_pstream *p, const uint8_t **payload, size_t *plen, char *why, size_t whylen) {
    if (!buf || !p || !payload || !plen) return TC_E_ARG;
    if (len < TC_CODED_HEADER || memcmp(buf, magic, 4) != 0) {
        WHY("coded file header missing (want magic %.4s, %d-byte header)", magic, TC_CODED_HEADER);
        return TC_E_HEADER;
    }
    if (buf[4] != TC_CODED_VERSION || buf[5] != coder_id || buf[6] || buf[7]) {
        WHY("coded file header altered: version %u coder %u reserved %u,%u (want %u, %u, 0, 0)", buf[4], buf[5],
            buf[6], buf[7], TC_CODED_VERSION, coder_id);
        return TC_E_HEADER;
    }
    if (memcmp(buf + 16, p->digest, TC_DIGEST) != 0) {
        WHY("coded file is bound to a different TPS1 digest");
        return TC_E_BINDING;
    }
    uint64_t n = tc_get_u64(buf + 8), pl = tc_get_u64(buf + 48);
    if (n != p->n) {
        WHY("coded file declares %" PRIu64 " symbols, TPS1 has %" PRIu64, n, p->n);
        return TC_E_COUNT;
    }
    if (pl > len - TC_CODED_HEADER) {
        WHY("payload declared %" PRIu64 " bytes, file carries %zu (truncated bitstream)", pl,
            len - TC_CODED_HEADER);
        return TC_E_TRUNC;
    }
    if (pl < len - TC_CODED_HEADER) {
        WHY("file carries %zu payload bytes, header declares %" PRIu64 " (trailing bytes)", len - TC_CODED_HEADER,
            pl);
        return TC_E_TRAIL;
    }
    *payload = buf + TC_CODED_HEADER;
    *plen = (size_t)pl;
    return TC_OK;
}
