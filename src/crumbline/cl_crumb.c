#include "cl_crumb.h"

#include <string.h>

/*
 * Conforms to aien-protocols specs/crumb-visible/CRUMB_READER_CONTRACT.md
 * 1.0.0: refusal codes and check order (section 6) follow the Rust reference.
 * Reading past the end of the buffer at any step is LENGTH.
 */

static bool lane_bytes_ok(uint8_t enc, uint8_t b) {
    if (enc == CL_ENC_DECIMAL) return b == 0;
    return b == 1 || b == 2 || b == 4 || b == 8;
}

/* Strict UTF-8 validity, same accept set as Rust std::str::from_utf8
 * (no overlong forms, no surrogates, nothing above U+10FFFF). */
static bool utf8_ok(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        size_t extra;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            return false;
        }
        if (extra > len - i - 1) return false;
        if (p[i + 1] < lo || p[i + 1] > hi) return false;
        for (size_t k = 2; k <= extra; ++k)
            if (p[i + k] < 0x80 || p[i + k] > 0xBF) return false;
        i += extra + 1;
    }
    return true;
}

static int parse_lanes(uint8_t enc, uint8_t lane_bytes, uint8_t arity, const uint8_t *p, size_t len, uint64_t *out) {
    if (enc == CL_ENC_RAW_LE) {
        if (len != (size_t)lane_bytes * arity) return CL_CRUMB_ERR_LANE;
        for (uint8_t l = 0; l < arity; ++l) {
            uint64_t v = 0;
            for (uint8_t k = 0; k < lane_bytes; ++k) v |= (uint64_t)p[l * lane_bytes + k] << (8u * k);
            out[l] = v;
        }
        return CL_CRUMB_OK;
    }
    /* Decimal: valid UTF-8 (else LANE), exactly arity parts split on single
     * spaces (else LANE), then per part in order: empty, non-digit or leading
     * zero is NONCANONICAL; a value above 2^64 - 1 is LANE. */
    if (!utf8_ok(p, len)) return CL_CRUMB_ERR_LANE;
    size_t parts = 1;
    for (size_t i = 0; i < len; ++i)
        if (p[i] == ' ') parts++;
    if (parts != arity) return CL_CRUMB_ERR_LANE;
    size_t pos = 0;
    for (uint8_t l = 0; l < arity; ++l) {
        size_t start = pos;
        while (pos < len && p[pos] != ' ') pos++;
        size_t digits = pos - start;
        if (digits == 0 || (digits > 1 && p[start] == '0')) return CL_CRUMB_ERR_NONCANONICAL;
        for (size_t k = start; k < pos; ++k)
            if (p[k] < '0' || p[k] > '9') return CL_CRUMB_ERR_NONCANONICAL;
        uint64_t v = 0;
        for (size_t k = start; k < pos; ++k) {
            uint64_t d = (uint64_t)(p[k] - '0');
            if (v > (UINT64_MAX - d) / 10) return CL_CRUMB_ERR_LANE;
            v = v * 10 + d;
        }
        out[l] = v;
        pos++; /* skip the separator (or step past the end after the last part) */
    }
    return CL_CRUMB_OK;
}

/* Read one length-prefixed example field (at most CL_CRUMB_MAX_FIELD bytes). */
static bool read_field(ClReader *r, const uint8_t **p, uint32_t *n) {
    *n = cl_r_u32(r);
    if (r->error || *n > CL_CRUMB_MAX_FIELD || *n > r->len - r->pos) return false;
    *p = r->buf + r->pos;
    r->pos += *n;
    return true;
}

int cl_crumb_decode(const uint8_t *buf, size_t len, ClCrumb *out) {
    memset(out, 0, sizeof(*out));
    ClReader r;
    cl_r_init(&r, buf, len);
    uint8_t magic[4];
    if (!cl_r_bytes(&r, magic, 4)) return CL_CRUMB_ERR_LENGTH;
    if (memcmp(magic, "CRB1", 4) != 0) return CL_CRUMB_ERR_MAGIC;
    uint16_t schema = cl_r_u16(&r);
    if (r.error) return CL_CRUMB_ERR_LENGTH;
    if (schema != 1) return CL_CRUMB_ERR_VERSION;
    out->encoding = cl_r_u8(&r);
    if (r.error) return CL_CRUMB_ERR_LENGTH;
    if (out->encoding != CL_ENC_DECIMAL && out->encoding != CL_ENC_RAW_LE) return CL_CRUMB_ERR_SHAPE;
    uint8_t flags = cl_r_u8(&r);
    if (r.error) return CL_CRUMB_ERR_LENGTH;
    if (flags & ~1u) return CL_CRUMB_ERR_NONCANONICAL;
    out->in_arity = cl_r_u8(&r);
    out->out_arity = cl_r_u8(&r);
    out->in_lane_bytes = cl_r_u8(&r);
    out->out_lane_bytes = cl_r_u8(&r);
    if (r.error) return CL_CRUMB_ERR_LENGTH;
    if (out->in_arity < 1 || out->in_arity > CL_CRUMB_MAX_LANES || out->out_arity < 1 ||
        out->out_arity > CL_CRUMB_MAX_LANES || !lane_bytes_ok(out->encoding, out->in_lane_bytes) ||
        !lane_bytes_ok(out->encoding, out->out_lane_bytes))
        return CL_CRUMB_ERR_SHAPE;
    out->n = cl_r_u32(&r);
    if (r.error || out->n == 0 || out->n > CL_CRUMB_MAX_EXAMPLES) return CL_CRUMB_ERR_LENGTH;
    for (uint32_t i = 0; i < out->n; ++i) {
        const uint8_t *ip, *op;
        uint32_t in_n, out_n;
        /* Both fields of an example are read before either is validated. */
        if (!read_field(&r, &ip, &in_n) || !read_field(&r, &op, &out_n)) return CL_CRUMB_ERR_LENGTH;
        int rc = parse_lanes(out->encoding, out->in_lane_bytes, out->in_arity, ip, in_n, out->in[i]);
        if (rc == CL_CRUMB_OK)
            rc = parse_lanes(out->encoding, out->out_lane_bytes, out->out_arity, op, out_n, out->out[i]);
        if (rc != CL_CRUMB_OK) return rc;
    }
    if (flags & 1u) {
        out->has_budget = true;
        out->budget_max_candidates = cl_r_u32(&r);
        out->budget_max_depth = cl_r_u32(&r);
        out->budget_max_oracle_queries = cl_r_u32(&r);
        out->budget_max_program_ops = cl_r_u32(&r);
    }
    if (!cl_r_done(&r)) return CL_CRUMB_ERR_LENGTH;
    return CL_CRUMB_OK;
}

uint64_t cl_crumb_out_mask(const ClCrumb *c) {
    return c->encoding == CL_ENC_RAW_LE ? cl_mask_bytes(c->out_lane_bytes) : UINT64_MAX;
}
