/*
 * cl_common.h -- Crumbline shared plumbing: canonical byte buffers, the
 * deterministic PRNG, width masks and domain-separated digests.
 *
 * Crumbline is the AIEN training curriculum of unlabeled input/output
 * observations. It is unrelated to the RFC-0001 Crumb Protocol (agent
 * coordination files in crumb-spec / spark-crumbs).
 *
 * This header is linked into BOTH the learner binary and the sealed teacher.
 * It must never name anything that exists only on the sealed side.
 */
#ifndef CL_COMMON_H
#define CL_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CL_DIGEST_BYTES 32

/* ---- canonical little-endian byte writer / reader -------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} ClWriter;

void cl_w_init(ClWriter *w, uint8_t *buf, size_t cap);
void cl_w_bytes(ClWriter *w, const void *p, size_t n);
void cl_w_u8(ClWriter *w, uint8_t v);
void cl_w_u16(ClWriter *w, uint16_t v);
void cl_w_u32(ClWriter *w, uint32_t v);
void cl_w_u64(ClWriter *w, uint64_t v);
/* Little-endian value truncated to `nbytes` (1..8). */
void cl_w_uN(ClWriter *w, uint64_t v, uint8_t nbytes);

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    bool error;
} ClReader;

void cl_r_init(ClReader *r, const uint8_t *buf, size_t len);
bool cl_r_bytes(ClReader *r, void *out, size_t n);
uint8_t cl_r_u8(ClReader *r);
uint16_t cl_r_u16(ClReader *r);
uint32_t cl_r_u32(ClReader *r);
uint64_t cl_r_u64(ClReader *r);
uint64_t cl_r_uN(ClReader *r, uint8_t nbytes);
static inline bool cl_r_done(const ClReader *r) { return !r->error && r->pos == r->len; }

/* ---- width masks ---------------------------------------------------- */

static inline uint64_t cl_mask_bytes(uint8_t nbytes) {
    return nbytes >= 8 ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << (8u * nbytes)) - 1ULL);
}

/* ---- deterministic PRNG: xoshiro256** seeded by splitmix64 ---------- */

typedef struct {
    uint64_t s[4];
} ClRng;

/* Seed from SHA-256(domain || u64 parts...) so every stream is named. */
void cl_rng_seed(ClRng *r, const char *domain, const uint64_t *parts, size_t nparts);
uint64_t cl_rng_next(ClRng *r);
/* Uniform in [0, n); n > 0. Rejection sampling, no modulo bias. */
uint64_t cl_rng_below(ClRng *r, uint64_t n);

/* ---- domain-separated SHA-256 --------------------------------------- */

void cl_digest(const char *domain, const uint8_t *data, size_t len, uint8_t out[CL_DIGEST_BYTES]);
void cl_hex(const uint8_t *bytes, size_t n, char *out /* 2n+1 */);

/* Search `hay` for `needle`; true if present. Used by the leak scanners. */
bool cl_contains(const uint8_t *hay, size_t hay_len, const void *needle, size_t needle_len);

#endif /* CL_COMMON_H */
