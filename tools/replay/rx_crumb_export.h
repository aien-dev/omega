/* rx_crumb_export.h -- copy World crumbs into an RXCLOG01 log (rxlog.h).
 * The only replay file that includes rx_world.h; it reads crumbs through
 * the public rx_world_crumb() and never changes the World. */
#ifndef OMEGA_RX_CRUMB_EXPORT_H
#define OMEGA_RX_CRUMB_EXPORT_H

#include "replay/rxlog.h"
#include "runtime/rx_world.h"

#include <stddef.h>

/* Convert one runtime crumb, field for field (digest and episode copied as
 * recorded, capability generations raw). */
void rxx_crumb(const RxCrumb *k, rxl_crumb *out);

/* Export state for one run. Capability generations are boot-time seeded by
 * the capability root (rx_caproot.c take_boot_gen), so two runs of the same
 * history never share them. The log stores every capability generation
 * relative to cap_base, the run's capability office generation, and the
 * crumb digests over that relative form (rxlog.h). Before rewriting a crumb
 * the exporter recomputes the runtime digest over the raw fields with the
 * independent rxl_crumb_digest and refuses the crumb if it differs, so the
 * verifier stays checked against the runtime's own crumb hash. */
typedef struct {
    uint64_t cap_base;
    uint64_t next;          /* next crumb id to export, from 1 */
    uint8_t (*raw)[32];     /* runtime digest of crumb id, at [id - 1] */
    uint8_t (*rel)[32];     /* log digest of crumb id, at [id - 1] */
    size_t n, cap;
} rxx_ctx;

void rxx_init(rxx_ctx *x, uint64_t cap_base);
void rxx_free(rxx_ctx *x);
/* Append crumbs [x->next, w->n_crumbs] to the log and advance x->next. Call
 * only while the World is quiescent. Returns 0, or -1 on a missing crumb, a
 * runtime digest the independent recomputation does not reproduce, a
 * capability generation below cap_base, or out of memory. */
int  rxx_append_crumbs(rxl_log *log, RxWorld *w, rxx_ctx *x);

#endif
