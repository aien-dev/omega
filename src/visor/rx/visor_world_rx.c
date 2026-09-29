/*
 * visor_world_rx.c -- visor_world_snapshot(): the only visor code that touches
 * an RxWorld (lane 6). Lives in src/visor/rx/ so the top-level src/visor wildcard does
 * NOT link it into the physics-free, runtime-free V1 `omega` binary. Built only
 * by mk/visor-world.mk.
 *
 * Read-only. Calls no runtime mutator and no authority-bearing function. Takes
 * the world mutex exactly once (the same lock rx_world_read takes) to copy a
 * few public scalars, releases it, then calls the public read functions, which
 * each lock on their own. Never holds the mutex across an rx_* call (the
 * mutex is not recursive). Never waits for quiescence.
 */
#include "visor_world.h"
#include "runtime/rx_world.h"

#include <stdio.h>
#include <string.h>

static const char *persist_name(RxPersist p) {
    switch (p) {
    case RX_PERSIST_EPHEMERAL: return "EPHEMERAL";
    case RX_PERSIST_RESIDENT: return "RESIDENT";
    case RX_PERSIST_DURABLE: return "DURABLE";
    default: return "?";
    }
}

typedef struct { uint32_t id, generation; RxPersist persist; bool placed; } ObjCopy;

int visor_world_snapshot(const struct RxWorld *cw, VisorWorldView *out) {
    if (!out) return -1;
    if (!cw) {
        memset(out, 0, sizeof(*out));
        out->state_name = "unattached";
        visor_world_gap_append(out, "no World given: snapshot refused (fail closed)");
        return -1;
    }
    /* The runtime's read API is not const-qualified and locking needs a
     * mutable mutex. The visor changes no World state through this pointer. */
    RxWorld *w = (RxWorld *)cw;

    memset(out, 0, sizeof(*out));
    out->attached = true;
    out->state_name = "resident";   /* fixed label: the World has no lifecycle state */

    ObjCopy objs[VISOR_WORLD_MAX_OBJECTS];
    size_t n_live = 0, n_copied = 0;
    uint32_t n_reactions, in_flight;

    pthread_mutex_lock(&w->mu);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        const RxObject *o = &w->objects[i];
        if (!o->live) continue;
        if (n_copied < VISOR_WORLD_MAX_OBJECTS) {
            objs[n_copied].id = o->id;
            objs[n_copied].generation = o->generation;
            objs[n_copied].persist = o->persist;
            objs[n_copied].placed = o->placed;
            n_copied++;
        }
        n_live++;
    }
    n_reactions = w->n_reactions;
    in_flight = w->in_flight;
    pthread_mutex_unlock(&w->mu);

    out->resident_object_count = n_live;
    out->reaction_count = n_reactions;
    out->active_work = in_flight;
    for (size_t i = 0; i < n_copied; i++) {
        snprintf(out->objects[i].id, sizeof(out->objects[i].id), "rx:%u@%u",
                 objs[i].id, objs[i].generation);
        out->objects[i].kind = persist_name(objs[i].persist);
        out->objects[i].state = objs[i].placed ? "live,placed" : "live";
    }
    out->object_count = n_copied;

    /* Public read calls, each under its own lock acquisition. */
    uint8_t d[32];
    rx_world_digest(w, d);
    memcpy(out->digest, "sha256:", 7);
    for (int i = 0; i < 32; i++)
        snprintf(out->digest + 7 + 2 * i, 3, "%02x", d[i]);
    out->publication_tail = rx_world_publication_tail(w);

    /* What the runtime cannot show through a coherent read today. */
    out->generation = 0;
    out->published_results = 0;
    out->staged_effects = 0;
    out->staged_effects_visible = false;
    if (n_live > n_copied)
        visor_world_gap_append(out, "object list truncated at 32");
    visor_world_gap_append(out, "generation: RxWorld holds no RxGenStore");
    visor_world_gap_append(out, "published results: kept per RcGate, not on RxWorld");
    visor_world_gap_append(out, "staged effects: RxGenStore only, not visible");
    visor_world_gap_append(out, "digest/tail read apart from objects, coherent only when quiescent");
    return 0;
}
