#ifndef OMEGA_NUMERIC_NATIVE_H
#define OMEGA_NUMERIC_NATIVE_H

#include "omega_numeric_lifecycle.h"
#include "m16_native.h"
#include <stdio.h>
#include <string.h>

/* Numeric launchers must use these wrappers together. The pinned M16 submit
 * returns failure before ringing the doorbell, so a submit failure can close.
 * Once submitted, a failed wait proves neither completion nor cancellation. */
static inline int omega_numeric_native_open(M16NativeContext *ctx) {
    memset(ctx, 0, sizeof *ctx);
    if (omega_numeric_native_acquire() != 0) {
        snprintf(ctx->rm.err, sizeof ctx->rm.err,
                 "numeric GPU owner busy or completion uncertain; submission refused");
        return -1;
    }
    int rc = m16_native_open(ctx);
    if (rc != 0) omega_numeric_native_release();
    return rc;
}

static inline int omega_numeric_native_close(M16NativeContext *ctx) {
    if (omega_numeric_native_uncertain()) return -1;
    int rc = m16_native_close(ctx);
    if (rc != 0) omega_numeric_native_poison();
    else omega_numeric_native_release();
    return rc;
}

static inline int omega_numeric_native_wait(volatile uint32_t *word,
                                             uint32_t want, uint64_t timeout_ms) {
    int rc = m16_native_wait_marker(word, want, timeout_ms);
    /* M16's wait uses a monotonic >= comparison. These are fixed payloads,
     * not sequence counters: a different word must never authorize readback. */
    if (rc == 0 && (!word || *word != want)) rc = -1;
    if (rc != 0) {
        omega_numeric_native_poison();
        fprintf(stderr, "GB10_COMPLETION_UNCERTAIN: numeric resources retained; "
                        "further numeric device work refused in this process\n");
    }
    return rc;
}
#endif
