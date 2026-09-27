#include "cl_common.h"
#include "sha256.h"

#include <string.h>

void cl_w_init(ClWriter *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

void cl_w_bytes(ClWriter *w, const void *p, size_t n) {
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    if (n) memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void cl_w_uN(ClWriter *w, uint64_t v, uint8_t nbytes) {
    uint8_t b[8];
    for (uint8_t i = 0; i < nbytes && i < 8; ++i) b[i] = (uint8_t)(v >> (8u * i));
    cl_w_bytes(w, b, nbytes);
}

void cl_w_u8(ClWriter *w, uint8_t v) { cl_w_uN(w, v, 1); }
void cl_w_u16(ClWriter *w, uint16_t v) { cl_w_uN(w, v, 2); }
void cl_w_u32(ClWriter *w, uint32_t v) { cl_w_uN(w, v, 4); }
void cl_w_u64(ClWriter *w, uint64_t v) { cl_w_uN(w, v, 8); }

void cl_r_init(ClReader *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->error = false;
}

bool cl_r_bytes(ClReader *r, void *out, size_t n) {
    if (r->error || n > r->len - r->pos) {
        r->error = true;
        if (out && n) memset(out, 0, n);
        return false;
    }
    if (n) memcpy(out, r->buf + r->pos, n);
    r->pos += n;
    return true;
}

uint64_t cl_r_uN(ClReader *r, uint8_t nbytes) {
    uint8_t b[8] = {0};
    if (nbytes > 8 || !cl_r_bytes(r, b, nbytes)) {
        r->error = true;
        return 0;
    }
    uint64_t v = 0;
    for (uint8_t i = 0; i < nbytes; ++i) v |= (uint64_t)b[i] << (8u * i);
    return v;
}

uint8_t cl_r_u8(ClReader *r) { return (uint8_t)cl_r_uN(r, 1); }
uint16_t cl_r_u16(ClReader *r) { return (uint16_t)cl_r_uN(r, 2); }
uint32_t cl_r_u32(ClReader *r) { return (uint32_t)cl_r_uN(r, 4); }
uint64_t cl_r_u64(ClReader *r) { return cl_r_uN(r, 8); }

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void cl_rng_seed(ClRng *r, const char *domain, const uint64_t *parts, size_t nparts) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1);
    for (size_t i = 0; i < nparts; ++i) {
        uint8_t b[8];
        for (int k = 0; k < 8; ++k) b[k] = (uint8_t)(parts[i] >> (8 * k));
        sha256_update(&c, b, 8);
    }
    uint8_t d[32];
    sha256_final(&c, d);
    uint64_t x = 0;
    for (int k = 0; k < 8; ++k) x |= (uint64_t)d[k] << (8 * k);
    for (int i = 0; i < 4; ++i) r->s[i] = splitmix64(&x);
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

uint64_t cl_rng_next(ClRng *r) {
    uint64_t *s = r->s;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

uint64_t cl_rng_below(ClRng *r, uint64_t n) {
    if (n <= 1) return 0;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
    uint64_t v;
    do {
        v = cl_rng_next(r);
    } while (v >= limit);
    return v % n;
}

void cl_digest(const char *domain, const uint8_t *data, size_t len, uint8_t out[CL_DIGEST_BYTES]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1);
    if (len) sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void cl_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = hx[bytes[i] >> 4];
        out[2 * i + 1] = hx[bytes[i] & 15];
    }
    out[2 * n] = 0;
}

bool cl_contains(const uint8_t *hay, size_t hay_len, const void *needle, size_t needle_len) {
    if (needle_len == 0 || needle_len > hay_len) return false;
    return memmem(hay, hay_len, needle, needle_len) != NULL;
}
