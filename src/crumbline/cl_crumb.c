#include "cl_crumb.h"

#include <string.h>

static bool lane_bytes_ok(uint8_t enc, uint8_t b) {
    if (enc == CL_ENC_DECIMAL) return b == 0;
    return b == 1 || b == 2 || b == 4 || b == 8;
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
    /* Decimal: arity canonical base-10 u64 values separated by single spaces. */
    size_t pos = 0;
    for (uint8_t l = 0; l < arity; ++l) {
        if (l > 0) {
            if (pos >= len || p[pos] != ' ') return CL_CRUMB_ERR_LANE;
            pos++;
        }
        size_t start = pos;
        uint64_t v = 0;
        while (pos < len && p[pos] >= '0' && p[pos] <= '9') {
            uint64_t d = (uint64_t)(p[pos] - '0');
            if (v > (UINT64_MAX - d) / 10) return CL_CRUMB_ERR_LANE;
            v = v * 10 + d;
            pos++;
        }
        size_t digits = pos - start;
        if (digits == 0 || (digits > 1 && p[start] == '0')) return CL_CRUMB_ERR_LANE;
        out[l] = v;
    }
    return pos == len ? CL_CRUMB_OK : CL_CRUMB_ERR_LANE;
}

int cl_crumb_decode(const uint8_t *buf, size_t len, ClCrumb *out) {
    memset(out, 0, sizeof(*out));
    ClReader r;
    cl_r_init(&r, buf, len);
    uint8_t magic[4];
    cl_r_bytes(&r, magic, 4);
    if (r.error || memcmp(magic, "CRB1", 4) != 0) return CL_CRUMB_ERR_MAGIC;
    if (cl_r_u16(&r) != 1) return CL_CRUMB_ERR_VERSION;
    out->encoding = cl_r_u8(&r);
    uint8_t flags = cl_r_u8(&r);
    out->in_arity = cl_r_u8(&r);
    out->out_arity = cl_r_u8(&r);
    out->in_lane_bytes = cl_r_u8(&r);
    out->out_lane_bytes = cl_r_u8(&r);
    if (r.error || (out->encoding != CL_ENC_DECIMAL && out->encoding != CL_ENC_RAW_LE) || (flags & ~1u) ||
        out->in_arity < 1 || out->in_arity > CL_CRUMB_MAX_LANES || out->out_arity < 1 ||
        out->out_arity > CL_CRUMB_MAX_LANES || !lane_bytes_ok(out->encoding, out->in_lane_bytes) ||
        !lane_bytes_ok(out->encoding, out->out_lane_bytes))
        return CL_CRUMB_ERR_SHAPE;
    out->n = cl_r_u32(&r);
    if (r.error || out->n == 0 || out->n > CL_CRUMB_MAX_EXAMPLES) return CL_CRUMB_ERR_LENGTH;
    for (uint32_t i = 0; i < out->n; ++i) {
        for (int side = 0; side < 2; ++side) {
            uint32_t n = cl_r_u32(&r);
            if (r.error || n > r.len - r.pos) return CL_CRUMB_ERR_LENGTH;
            const uint8_t *p = r.buf + r.pos;
            r.pos += n;
            int rc = side == 0 ? parse_lanes(out->encoding, out->in_lane_bytes, out->in_arity, p, n, out->in[i])
                               : parse_lanes(out->encoding, out->out_lane_bytes, out->out_arity, p, n, out->out[i]);
            if (rc != CL_CRUMB_OK) return rc;
        }
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
