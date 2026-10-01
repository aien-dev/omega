/* rx_crumb_export.h -- copy World crumbs into an RXCLOG01 log (rxlog.h).
 * The only replay file that includes rx_world.h; it reads crumbs through
 * the public rx_world_crumb() and never changes the World. */
#ifndef OMEGA_RX_CRUMB_EXPORT_H
#define OMEGA_RX_CRUMB_EXPORT_H

#include "replay/rxlog.h"
#include "runtime/rx_world.h"

/* Convert one runtime crumb, field for field (digest and episode copied as
 * recorded, not recomputed). */
void rxx_crumb(const RxCrumb *k, rxl_crumb *out);
/* Append crumbs [*next, w->n_crumbs] to the log and advance *next. Call
 * only while the World is quiescent. Returns 0, or -1 on a missing crumb. */
int  rxx_append_crumbs(rxl_log *log, RxWorld *w, uint64_t *next);

#endif
