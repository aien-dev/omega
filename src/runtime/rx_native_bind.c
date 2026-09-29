/*
 * rx_native_bind.c -- point a reaction world at the native authority view.
 * The Linux oracle is not started and is not deleted.
 */
#include "rx_world.h"
#include "aienos_cap.h"
#include "rx_argus.h"

#include <stddef.h>

_Static_assert(sizeof(AienosCapRef) == sizeof(RxCapRef), "capability reference width");
_Static_assert(sizeof(AienosCapEntry) == sizeof(RxCapEntry), "capability entry width");
_Static_assert(offsetof(AienosCapEntry, resource) == offsetof(RxCapEntry, resource),
               "resource field moved");
_Static_assert(offsetof(AienosCapEntry, minted_by_generation) ==
                   offsetof(RxCapEntry, minted_by_generation),
               "entry tail moved");

static int native_validate(const void *ctx, RxCapRef ref, uint32_t subject, uint64_t resource,
                           uint32_t rights, RxCapEntry *out) {
    AienosCapRef cap = { ref.cap_id, ref.generation };
#if RX_ARGUS
    /* ARGUS v1.1: order key read before the validate; a success is counted in
     * this thread's use table, a failure is a full event (generation u32 -> u64). */
    uint64_t argus_key = rx_argus_use_begin();
#endif
    int rc = aienos_cap_validate(ctx, cap, subject, resource, rights, (AienosCapEntry *)out);
#if RX_ARGUS
    rx_argus_use_end(argus_key, ctx, subject, ref.cap_id, (uint64_t)ref.generation, resource, rc);
#endif
    return rc;
}

static int native_inspect(const void *ctx, RxCapRef ref, RxCapEntry *out) {
    AienosCapRef cap = { ref.cap_id, ref.generation };
    return aienos_cap_inspect(ctx, cap, (AienosCapEntry *)out);
}

int rx_world_init_native(RxWorld *w, const AienosCapView *view, uint32_t n_workers,
                         uint64_t crumb_cap) {
    if (!view) return RX_ERR_ARG;
    return rx_world_init_with_auth(w, NULL, view, native_validate, native_inspect, n_workers,
                                   crumb_cap);
}

/* R15 sequential reference on the same native authority (rx_seq_reference.h). */
int rx_world_init_sequential_reference(RxWorld *w, const void *auth_ctx,
                                       RxAuthValidateFn validate, RxAuthInspectFn inspect,
                                       uint64_t crumb_cap);
int rx_world_init_native_sequential_reference(RxWorld *w, const AienosCapView *view,
                                              uint64_t crumb_cap) {
    if (!view) return RX_ERR_ARG;
    return rx_world_init_sequential_reference(w, view, native_validate, native_inspect,
                                              crumb_cap);
}
