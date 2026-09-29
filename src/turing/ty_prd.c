#include "turing/ty_prd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRD_HDR 8u
#define PRD_REC 24u /* index, family, loc, scale */

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

int ty_prd_validate(const tyq_pred *p)
{
    if (!p)
        return TYQ_E_ARG;
    if (p->family != TYQ_FAM_GAUSS)
        return TYQ_FAIL_PROTOCOL;
    if (!isfinite(p->loc) || !isfinite(p->scale) || !(p->scale > 0.0))
        return TYQ_FAIL_PROTOCOL;
    return TYQ_OK;
}

void ty_prd_free(ty_prd *p)
{
    if (!p)
        return;
    free(p->rec);
    p->rec = NULL;
    p->n = 0;
}

int ty_prd_parse(const uint8_t *buf, size_t len, uint32_t first_index, uint32_t count, ty_prd *out)
{
    if (!buf || !out || count == 0)
        return TYQ_E_ARG;
    out->n = 0;
    out->first_index = first_index;
    out->rec = NULL;
    if ((uint64_t)first_index + count > UINT32_MAX)
        return TYQ_E_ARG;
    if (len < PRD_HDR || memcmp(buf, "PRD1", 4) != 0 || rd32(buf + 4) != 1u)
        return TYQ_FAIL_PROTOCOL;
    /* Every accepted record is exactly PRD_REC bytes, so the size is exact. */
    if (len - PRD_HDR != (size_t)count * PRD_REC) {
        /* Still a refusal whatever the cause: truncated, trailing or missing. */
        return TYQ_FAIL_PROTOCOL;
    }
    tyq_pred *rec = calloc(count, sizeof *rec);
    if (!rec)
        return TYQ_E_IO;
    const uint8_t *q = buf + PRD_HDR;
    for (uint32_t i = 0; i < count; i++, q += PRD_REC) {
        tyq_pred *r = &rec[i];
        r->index = rd32(q);
        r->family = rd32(q + 4);
        r->loc = rdf(q + 8);
        r->scale = rdf(q + 16);
        if (r->index != first_index + i || ty_prd_validate(r) != TYQ_OK) {
            free(rec);
            return TYQ_FAIL_PROTOCOL;
        }
    }
    out->n = count;
    out->rec = rec;
    return TYQ_OK;
}

int ty_prd_read(const char *path, uint32_t first_index, uint32_t count, ty_prd *out)
{
    if (!path || !out)
        return TYQ_E_ARG;
    FILE *f = fopen(path, "rb");
    if (!f)
        return TYQ_E_IO;
    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return TYQ_E_IO;
    }
    for (;;) {
        if (len == cap) {
            if (cap > ((size_t)1 << 30)) { /* far beyond any legal file */
                free(buf);
                fclose(f);
                return TYQ_FAIL_PROTOCOL;
            }
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                fclose(f);
                return TYQ_E_IO;
            }
            buf = nb;
            cap *= 2;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0)
            break;
    }
    int err = ferror(f);
    fclose(f);
    int rc = err ? TYQ_E_IO : ty_prd_parse(buf, len, first_index, count, out);
    free(buf);
    return rc;
}

int ty_prd_encode(const ty_prd *p, uint8_t **buf, size_t *len)
{
    if (!p || !buf || !len || (p->n && !p->rec))
        return TYQ_E_ARG;
    *buf = NULL;
    *len = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        if (ty_prd_validate(&p->rec[i]) != TYQ_OK || p->rec[i].index != p->first_index + i)
            return TYQ_FAIL_PROTOCOL;
    }
    size_t total = PRD_HDR + (size_t)p->n * PRD_REC;
    uint8_t *b = malloc(total);
    if (!b)
        return TYQ_E_IO;
    memcpy(b, "PRD1", 4);
    wr32(b + 4, 1u);
    uint8_t *q = b + PRD_HDR;
    for (uint32_t i = 0; i < p->n; i++, q += PRD_REC) {
        wr32(q, p->rec[i].index);
        wr32(q + 4, p->rec[i].family);
        wrf(q + 8, p->rec[i].loc);
        wrf(q + 16, p->rec[i].scale);
    }
    *buf = b;
    *len = total;
    return TYQ_OK;
}
