/*
 * visor_world.h -- Omega Visor V1, lane 6: a read-only view of the resident World.
 *
 * VISOR VIEW != RUNTIME OWNERSHIP.
 * A VisorWorldView is a bounded, value-only copy taken for display. It holds no
 * pointer into the runtime, no lock, no capability and no authority. Holding a
 * view grants nothing; changing a view changes nothing. The runtime (RxWorld,
 * its capability root, its generation store) stays the only owner of state.
 * The visor never calls a runtime mutator: no rx_world_create/retire/
 * add_reaction/publish_external/bind_capability, no rx_capadmin_*, no
 * rx_gen_propose/mutate_object/promote, no rx_resident_* mutators.
 *
 * Build split (the V1 `omega` binary is physics-free and does NOT link the
 * runtime):
 *   visor_world.c        types + formatters + visor_world_unattached(). No rx_*
 *                        symbol, no runtime header. Linked into build/omega.
 *   rx/visor_world_rx.c  visor_world_snapshot(), the only code that touches
 *                        RxWorld. Built only by mk/visor-world.mk (test target),
 *                        never by the src/visor/ wildcard.
 * This header forward-declares struct RxWorld so it never pulls in pthread.h
 * or runtime headers.
 *
 * Where each field comes from (visor_world_snapshot):
 *   (a) public rx_* read calls: digest (rx_world_digest), publication_tail
 *       (rx_world_publication_tail), per-object cross-check (rx_world_read).
 *   (b) ONE acquisition of the same world mutex rx_world_read takes, copying
 *       only public RxWorld scalars: objects[i].{live,id,generation,persist,
 *       placed}, n_reactions, in_flight. Nothing else (subscriptions, ready
 *       queues, deferred wakes, crumbs, coherent image) is touched.
 *   (c) nothing: generation, published_results, staged_effects stay 0 and
 *       gap_note says why. state_name is a fixed label, not a runtime state.
 * digest and publication_tail are separate lock acquisitions from (b); they
 * are coherent with the object list only when the World is quiescent.
 *
 * Extension points (later views attach to this same snapshot family; each is
 * a separate value-only view struct + snapshot + text/json formatter under the
 * same rules: bounded, deterministic, fail closed, gap_note instead of guess):
 *   - VisorJSpaceView     J-Space, over rx_jspace.h        -> visor_jspace_snapshot()
 *   - VisorCortexView     Cortex projection, over rx_cortex.h / rx_projection.h
 *                                                          -> visor_cortex_snapshot()
 *   - VisorGenerationView active generation / staged candidate, over an
 *                         RxGenStore (rx_gen_active, rx_gen_read_blob); fills
 *                         VisorWorldView.generation and staged_effects.
 *   - VisorContractView   published typed results, over rx_contract.h RcGate;
 *                         fills VisorWorldView.published_results.
 */
#ifndef OMEGA_VISOR_WORLD_H
#define OMEGA_VISOR_WORLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VISOR_WORLD_MAX_OBJECTS 32

struct RxWorld;

typedef struct {
    uint64_t generation;            /* active generation; 0 = not observable here */
    const char *state_name;         /* static label; never a pointer into the runtime */
    size_t resident_object_count;   /* true live-object total (may exceed object_count) */
    size_t active_work;             /* reactions READY + RUNNING + PUBLISHING */
    size_t reaction_count;          /* registered reactions */
    size_t published_results;       /* 0 = not observable here (see gap_note) */
    size_t staged_effects;          /* 0 = not observable here (see gap_note) */
    bool staged_effects_visible;    /* false: the runtime grants no view of staged effects */
    uint64_t publication_tail;      /* next publication-ring sequence */
    char digest[72];                /* "sha256:<64 hex>" world digest, "" if none */
    bool attached;                  /* false: no resident World in this session */
    char gap_note[256];             /* what the runtime cannot show coherently */
    struct {
        char id[72];                /* "rx:<slot>@<generation>" */
        const char *kind;           /* persistence class, static string */
        const char *state;          /* "live" | "live,placed", static string */
    } objects[VISOR_WORLD_MAX_OBJECTS];
    size_t object_count;
} VisorWorldView;

/* attached=false; gap_note says the V1 binary does not link the runtime. */
void visor_world_unattached(VisorWorldView *out);

/* Defined in src/visor/rx/visor_world_rx.c (runtime-linked builds only).
 * Returns 0 on success. Returns -1 and leaves *out unattached (fail closed)
 * when w or out is NULL. Never waits for quiescence; never mutates w. */
int visor_world_snapshot(const struct RxWorld *w, VisorWorldView *out);

/* Deterministic renderings. Return bytes written (excluding NUL), or -1 on a
 * NULL argument or when the output would not fit (out is then "" if n > 0). */
int visor_world_format_text(const VisorWorldView *v, char *out, size_t n);
int visor_world_format_json(const VisorWorldView *v, char *out, size_t n);

/* Append a clause to gap_note, separated by "; ", truncating safely. */
void visor_world_gap_append(VisorWorldView *v, const char *note);

#endif /* OMEGA_VISOR_WORLD_H */
