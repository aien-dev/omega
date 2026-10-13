/*
 * osc_alloc.c -- see osc_alloc.h.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *libc_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n ? n : 1);
}
static void *libc_realloc(void *ctx, void *p, size_t old_n, size_t new_n)
{
    (void)ctx;
    (void)old_n;
    return realloc(p, new_n ? new_n : 1);
}
static void libc_free(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n;
    free(p);
}
static const OscAlloc k_libc = {libc_alloc, libc_realloc, libc_free, NULL};

const OscAlloc *osc_alloc_libc(void) { return &k_libc; }

void *osc_alloc_zero(const OscAlloc *a, size_t n)
{
    void *p = a->alloc(a->ctx, n);
    if (p) memset(p, 0, n);
    return p;
}

/* ------------------------------------------------------------ meter */
#define MHDR 16 /* header bytes before each block: the recorded size, padded to keep 16-byte alignment */

void osc_alloc_meter_init(OscAllocMeter *m, const OscAlloc *base, size_t budget, size_t fail_at)
{
    memset(m, 0, sizeof *m);
    m->base = base ? base : osc_alloc_libc();
    m->budget = budget;
    m->fail_at = fail_at;
}

static size_t *hdr_of(void *p) { return (size_t *)((uint8_t *)p - MHDR); }

/* would taking `grow` more bytes pass the budget or the fail_at-th counted call? */
static int meter_refuses(OscAllocMeter *m, size_t grow)
{
    m->calls++;
    if (m->fail_at && m->calls == m->fail_at) return 1;
    if (grow > SIZE_MAX - MHDR) return 1;
    if (m->budget && (grow > m->budget || m->live > m->budget - grow)) return 1;
    return 0;
}

static void *meter_alloc(void *ctx, size_t n)
{
    OscAllocMeter *m = ctx;
    m->total_calls++;
    if (meter_refuses(m, n)) { m->refused++; return NULL; }
    uint8_t *raw = m->base->alloc(m->base->ctx, n + MHDR);
    if (!raw) { m->refused++; return NULL; }
    memcpy(raw, &n, sizeof n);
    m->live += n;
    if (m->live > m->peak) m->peak = m->live;
    return raw + MHDR;
}

static void *meter_realloc(void *ctx, void *p, size_t old_n, size_t new_n)
{
    OscAllocMeter *m = ctx;
    if (!p) return meter_alloc(ctx, new_n);
    m->total_calls++;
    size_t rec = *hdr_of(p);
    if (rec != old_n) m->bad++;
    if (new_n > rec && meter_refuses(m, new_n - rec)) { m->refused++; return NULL; }
    uint8_t *raw = m->base->realloc(m->base->ctx, hdr_of(p), rec + MHDR, new_n + MHDR);
    if (!raw) { m->refused++; return NULL; }
    memcpy(raw, &new_n, sizeof new_n);
    m->live = m->live - rec + new_n;
    if (m->live > m->peak) m->peak = m->live;
    return raw + MHDR;
}

static void meter_free(void *ctx, void *p, size_t n)
{
    OscAllocMeter *m = ctx;
    m->total_calls++;
    if (!p) return;
    size_t rec = *hdr_of(p);
    if (rec != n) m->bad++;
    m->live -= rec;
    m->base->free(m->base->ctx, hdr_of(p), rec + MHDR);
}

OscAlloc osc_alloc_meter(OscAllocMeter *m)
{
    OscAlloc a = {meter_alloc, meter_realloc, meter_free, m};
    return a;
}
