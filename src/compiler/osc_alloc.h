/*
 * osc_alloc.h -- explicit allocation context for the OSC compiler (docs/osc/OSC-1-DESIGN.md,
 * "Explicit allocation"). Every byte the compiler takes from the heap goes through one OscAlloc
 * the caller hands in, so out-of-memory behaviour is testable and peak memory is bounded.
 * Zig-style: sizes travel with the call (free and realloc get the size the block was allocated
 * with), so an allocator needs no per-block header and a test allocator can check the sizes.
 * Contract:
 *   alloc(ctx, n)            n bytes, uninitialised, 16-byte aligned; NULL on refusal.
 *   realloc(ctx, p, old_n, new_n)
 *                            p == NULL means alloc. On NULL the old block is untouched and still owned
 *                            by the caller (libc semantics). Contents up to min(old_n, new_n) are kept.
 *   free(ctx, p, n)          n = the size p was last allocated or reallocated with; p == NULL is a no-op.
 * The compiler never assumes zeroed memory (it clears what it needs, see osc_alloc_zero), never
 * keeps a block after the call that took it returns (except OscCode.code, released by osc_cg_free),
 * and answers a refusal with a clean failure: OSC_DIAG_CAPACITY from the front end, -1 with
 * "out of memory" in err from the validator and back end. Output never depends on the allocator.
 * An OscAlloc passed to a compiler entry point must outlive every block taken from it.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_ALLOC_H
#define OSC_ALLOC_H

#include <stddef.h>

typedef struct OscAlloc {
    void *(*alloc)(void *ctx, size_t n);
    void *(*realloc)(void *ctx, void *p, size_t old_n, size_t new_n);
    void (*free)(void *ctx, void *p, size_t n);
    void *ctx;
} OscAlloc;

/* malloc / realloc / free. What every non-_a entry point uses; a NULL OscAlloc* means this. */
const OscAlloc *osc_alloc_libc(void);

/* alloc + memset 0 (the calloc the checker and lowerer used to call). */
void *osc_alloc_zero(const OscAlloc *a, size_t n);

/* Test / budget wrapper, usable by any caller. Sits over `base`, keeps a 16-byte size header in
 * front of every block so a free or realloc with the wrong size is caught (bad). Counting:
 *   calls     counted attempts: every alloc and every growing realloc (the fail_at clock)
 *   total_calls  every call through the meter, free and shrinking realloc included
 *   live/peak user bytes held now / at the high-water mark (logical sizes, headers excluded)
 * Refusal (NULL, refused++): the fail_at-th counted call (fail_at 0 = never), or a call that would
 * take live above budget (budget 0 = none), or base refusing. A compile that leaves live != 0
 * leaked; one with bad != 0 passed a wrong size. */
typedef struct {
    const OscAlloc *base;
    size_t live, peak, total_calls, budget, fail_at;
    size_t calls, refused, bad;
} OscAllocMeter;

void osc_alloc_meter_init(OscAllocMeter *m, const OscAlloc *base, size_t budget, size_t fail_at);
OscAlloc osc_alloc_meter(OscAllocMeter *m); /* the allocator view; m must outlive it */

#endif /* OSC_ALLOC_H */
