/*
 * rx_native_bind.c -- point a reaction world at the native authority view.
 * The Linux oracle is not started and is not deleted.
 */
#include "rx_world.h"
#include "aienos_cap.h"

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
    return aienos_cap_validate(ctx, cap, subject, resource, rights, (AienosCapEntry *)out);
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
