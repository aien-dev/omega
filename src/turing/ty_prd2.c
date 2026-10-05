#include "turing/ty_prd2.h"
#include "turing/ty_math.h"

#include <fenv.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define P2_HDR 16u
#define P2_REC 16u  /* index, family, K, reserved */
#define P2_CMP 24u  /* pi, loc, scale */

const char *tyq_prd2_reason_name(int r)
{
    switch (r) {
    case PRD2_R_NONE: return "NONE";
    case PRD2_R_MAGIC: return "MAGIC";
    case PRD2_R_VERSION: return "VERSION";
    case PRD2_R_SIZE: return "SIZE";
    case PRD2_R_COUNT: return "COUNT";
    case PRD2_R_INDEX: return "INDEX";
    case PRD2_R_FAMILY: return "FAMILY";
    case PRD2_R_K: return "K";
    case PRD2_R_RESERVED: return "RESERVED";
    case PRD2_R_WEIGHT_NONFINITE: return "WEIGHT_NONFINITE";
    case PRD2_R_WEIGHT_RANGE: return "WEIGHT_RANGE";
    case PRD2_R_WEIGHT_SUM: return "WEIGHT_SUM";
    case PRD2_R_GAUSS_WEIGHT: return "GAUSS_WEIGHT";
    case PRD2_R_LOC: return "LOC";
    case PRD2_R_SCALE: return "SCALE";
    default: return "?";
    }
}

static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static double rdf(const uint8_t *b)
{
    uint64_t u = 0;
    double d;
    for (int i = 7; i >= 0; i--)
        u = (u << 8) | b[i];
    memcpy(&d, &u, 8);
    return d;
}
static void wr32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}
static void wrf(uint8_t *b, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(u >> (8 * i));
}

static int refuse(int *reason, int r)
{
    if (reason)
        *reason = r;
    return TYQ_FAIL_PROTOCOL;
}

int ty_prd2_validate(const tyq_pred *p, int *reason)
{
    if (reason)
        *reason = PRD2_R_NONE;
    if (!p)
        return TYQ_E_ARG;
    if (p->family != TYQ_FAM_GAUSS && p->family != TYQ_FAM_MIX)
        return refuse(reason, PRD2_R_FAMILY);
    if (p->family == TYQ_FAM_GAUSS && p->K != 1u)
        return refuse(reason, PRD2_R_K);
    if (p->family == TYQ_FAM_MIX && (p->K < 1u || p->K > TYQ_KMAX))
        return refuse(reason, PRD2_R_K);
    double sum = 0.0;
    for (uint32_t j = 0; j < p->K; j++) {
        const tyq_comp *c = &p->comp[j];
        if (!isfinite(c->pi))
            return refuse(reason, PRD2_R_WEIGHT_NONFINITE);
        if (!(c->pi > 0.0) || c->pi > 1.0)
            return refuse(reason, PRD2_R_WEIGHT_RANGE);
        if (!isfinite(c->loc))
            return refuse(reason, PRD2_R_LOC);
        if (!isfinite(c->scale) || !(c->scale > 0.0))
            return refuse(reason, PRD2_R_SCALE);
        sum += c->pi;
    }
    if (p->family == TYQ_FAM_GAUSS) {
        if (p->comp[0].pi != 1.0)
            return refuse(reason, PRD2_R_GAUSS_WEIGHT);
        if (p->loc != p->comp[0].loc || p->scale != p->comp[0].scale)
            return refuse(reason, PRD2_R_LOC);
    } else if (fabs(sum - 1.0) > TYQ_NORM_TOL) {
        return refuse(reason, PRD2_R_WEIGHT_SUM);
    }
    return TYQ_OK;
}

void ty_prd2_free(ty_prd2 *p)
{
    if (!p)
        return;
    free(p->rec);
    p->rec = NULL;
    p->n = 0;
}

int ty_prd2_parse(const uint8_t *buf, size_t len, uint32_t first_index, uint32_t count,
                  ty_prd2 *out, int *reason)
{
    if (reason)
        *reason = PRD2_R_NONE;
    if (!buf || !out || count == 0)
        return TYQ_E_ARG;
    out->n = 0;
    out->first_index = first_index;
    out->rec = NULL;
    if ((uint64_t)first_index + count > UINT32_MAX)
        return TYQ_E_ARG;
    if (len < P2_HDR)
        return refuse(reason, PRD2_R_SIZE);
    if (memcmp(buf, "PRD2", 4) != 0)
        return refuse(reason, PRD2_R_MAGIC);
    if (rd32(buf + 4) != 2u)
        return refuse(reason, PRD2_R_VERSION);
    if (rd32(buf + 8) != count || rd32(buf + 12) != first_index)
        return refuse(reason, PRD2_R_COUNT);
    tyq_pred *rec = calloc(count, sizeof *rec);
    if (!rec)
        return TYQ_E_IO;
    size_t off = P2_HDR;
    for (uint32_t i = 0; i < count; i++) {
        if (len - off < P2_REC) {
            free(rec);
            return refuse(reason, PRD2_R_SIZE);
        }
        tyq_pred *r = &rec[i];
        const uint8_t *q = buf + off;
        r->index = rd32(q);
        r->family = rd32(q + 4);
        r->K = rd32(q + 8);
        uint32_t reserved = rd32(q + 12);
        off += P2_REC;
        if (r->index != first_index + i) {
            free(rec);
            return refuse(reason, PRD2_R_INDEX);
        }
        if (reserved != 0u) {
            free(rec);
            return refuse(reason, PRD2_R_RESERVED);
        }
        if (r->K < 1u || r->K > TYQ_KMAX) {
            free(rec);
            return refuse(reason, PRD2_R_K);
        }
        if (len - off < (size_t)r->K * P2_CMP) {
            free(rec);
            return refuse(reason, PRD2_R_SIZE);
        }
        for (uint32_t j = 0; j < r->K; j++, off += P2_CMP) {
            r->comp[j].pi = rdf(buf + off);
            r->comp[j].loc = rdf(buf + off + 8);
            r->comp[j].scale = rdf(buf + off + 16);
        }
        if (r->family == TYQ_FAM_GAUSS) {
            r->loc = r->comp[0].loc;
            r->scale = r->comp[0].scale;
        }
        int rr;
        if (ty_prd2_validate(r, &rr) != TYQ_OK) {
            free(rec);
            return refuse(reason, rr);
        }
    }
    if (off != len) {
        free(rec);
        return refuse(reason, PRD2_R_SIZE);
    }
    out->n = count;
    out->rec = rec;
    return TYQ_OK;
}

int ty_prd2_encode(const ty_prd2 *p, uint8_t **buf, size_t *len)
{
    if (!p || !buf || !len || (p->n && !p->rec))
        return TYQ_E_ARG;
    *buf = NULL;
    *len = 0;
    size_t total = P2_HDR;
    for (uint32_t i = 0; i < p->n; i++) {
        if (ty_prd2_validate(&p->rec[i], NULL) != TYQ_OK || p->rec[i].index != p->first_index + i)
            return TYQ_FAIL_PROTOCOL;
        total += P2_REC + (size_t)p->rec[i].K * P2_CMP;
    }
    uint8_t *b = malloc(total);
    if (!b)
        return TYQ_E_IO;
    memcpy(b, "PRD2", 4);
    wr32(b + 4, 2u);
    wr32(b + 8, p->n);
    wr32(b + 12, p->first_index);
    size_t off = P2_HDR;
    for (uint32_t i = 0; i < p->n; i++) {
        const tyq_pred *r = &p->rec[i];
        wr32(b + off, r->index);
        wr32(b + off + 4, r->family);
        wr32(b + off + 8, r->K);
        wr32(b + off + 12, 0u);
        off += P2_REC;
        for (uint32_t j = 0; j < r->K; j++, off += P2_CMP) {
            wrf(b + off, r->comp[j].pi);
            wrf(b + off + 8, r->comp[j].loc);
            wrf(b + off + 16, r->comp[j].scale);
        }
    }
    *buf = b;
    *len = total;
    return TYQ_OK;
}

int ty_qcont2_bits(const tyq_pred *p, int64_t k, double *bits, int *floor_hit)
{
    if (!p || !bits)
        return TYQ_E_ARG;
    if (ty_prd2_validate(p, NULL) != TYQ_OK)
        return TYQ_FAIL_PROTOCOL;
    if (p->family == TYQ_FAM_GAUSS)
        return ty_qcont_bits(p, k, bits, floor_hit);
    if (fegetround() != FE_TONEAREST)
        return TYQ_FAIL_PROTOCOL;
    double b[TYQ_KMAX];
    int anyhit = 0;
    double m = INFINITY;
    for (uint32_t j = 0; j < p->K; j++) {
        tyq_pred g;
        memset(&g, 0, sizeof g);
        g.index = p->index;
        g.family = TYQ_FAM_GAUSS;
        g.loc = p->comp[j].loc;
        g.scale = p->comp[j].scale;
        int hit = 0;
        int rc = ty_qcont_bits(&g, k, &b[j], &hit);
        if (rc != TYQ_OK)
            return rc;
        anyhit |= hit;
        if (b[j] < m)
            m = b[j];
    }
    double acc = 0.0;
    for (uint32_t j = 0; j < p->K; j++)
        acc += p->comp[j].pi * exp2(-(b[j] - m));
    double v = m - log2(acc);
    if (!isfinite(v))
        return TYQ_FAIL_PROTOCOL;
    *bits = v;
    if (floor_hit)
        *floor_hit = anyhit;
    return TYQ_OK;
}

int ty_qcont2_point_ub(const tyq_pred *p, int64_t k, int64_t *ub, int *floor_hit)
{
    if (!ub)
        return TYQ_E_ARG;
    if (p && p->family == TYQ_FAM_GAUSS && ty_prd2_validate(p, NULL) == TYQ_OK)
        return ty_qcont_point_ub(p, k, ub, floor_hit);
    double bits;
    int rc = ty_qcont2_bits(p, k, &bits, floor_hit);
    if (rc != TYQ_OK)
        return rc;
    double s = 1e6 * bits;
    if (!isfinite(s) || s > 9.0e18 || s < -9.0e18)
        return TYQ_FAIL_PROTOCOL;
    *ub = llrint(s);
    return TYQ_OK;
}

int ty_qcont2_sum_ub(const tyq_pred *p, const int64_t *k, size_t n,
                     int64_t *ld_ub, uint64_t *floor_hits)
{
    if (!p || !k || !ld_ub || n == 0)
        return TYQ_E_ARG;
    int64_t sum = 0;
    uint64_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t ub;
        int hit = 0;
        int rc = ty_qcont2_point_ub(&p[i], k[i], &ub, &hit);
        if (rc != TYQ_OK)
            return rc;
        if (ty_add(sum, ub, &sum) != TY_OK)
            return TYQ_FAIL_PROTOCOL;
        hits += (uint64_t)hit;
    }
    *ld_ub = sum;
    if (floor_hits)
        *floor_hits = hits;
    return TYQ_OK;
}
