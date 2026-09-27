/*
 * rx_world.h -- first heartbeat of the resident reaction runtime (ADR 0016).
 *
 * One shared object world. Reactions declare what wakes them, what they read,
 * what they write and which capabilities they need. A dependency index maps a
 * (object, field-mask) change to the interested reactions only; nothing scans
 * the graph. Worker threads run whatever is READY, in admission order; they
 * never decide the semantic sequence. A reaction computes against a versioned
 * snapshot, then publishes atomically after re-validating its inputs and its
 * capabilities. Every outcome leaves a content-addressed causal crumb.
 *
 * This is the simple human reference (ADR 0016 §56 step 2): one world mutex,
 * bounded fixed-size tables. It is deliberately not optimized.
 *
 * Covered here: R1 subset (logical ids, generations, versions, persistence
 * class, atomic publication, stale-reference rejection), R3 (reaction core),
 * R4 subset (causal crumbs + ancestry). Not covered: R2 GPU ABI, R5 modulation
 * beyond priority classes, R6 storm control beyond wake coalescing, R7 native
 * root, R9 generation barrier.
 */
#ifndef RX_WORLD_H
#define RX_WORLD_H

#include "rx_caproot.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define RX_MAX_OBJECTS      256u
#define RX_MAX_FIELDS       8u
#define RX_MAX_REACTIONS    256u
#define RX_MAX_DEPS         8u
#define RX_MAX_WRITES       8u
#define RX_MAX_CAPS         8u
#define RX_MAX_MUTATIONS    16u
#define RX_MAX_SUBS         512u   /* subscriptions per object */
#define RX_MAX_WORKERS      16u
#define RX_MAX_PARENTS      (RX_MAX_DEPS * RX_MAX_FIELDS + 1u)
#define RX_PRIORITY_CLASSES 7u

#define RX_ALL_FIELDS       ((uint64_t)((1u << RX_MAX_FIELDS) - 1u))
#define RX_FIELD(i)         ((uint64_t)1u << (i))

/* Faculties (logical write domains, not memory owners). */
enum { RX_FACULTY_EXTERNAL = 0, RX_FACULTY_AIEN, RX_FACULTY_OMEGA, RX_FACULTY_AEGIS };

/* Priority classes (ADR 0016 §9), highest first. */
enum {
    RX_PRIO_CRITICAL = 0, RX_PRIO_INTERACTIVE, RX_PRIO_FOREGROUND, RX_PRIO_LEARNING,
    RX_PRIO_MAINTENANCE, RX_PRIO_SPECULATIVE, RX_PRIO_BACKGROUND
};

/* Persistence class (ADR 0016 §19; ADR 0015 §2 mapping in ADR 0016 Part I). */
typedef enum { RX_PERSIST_EPHEMERAL = 0, RX_PERSIST_RESIDENT, RX_PERSIST_DURABLE } RxPersist;

/* Reaction lifecycle (ADR 0016 §6). Every activation ends back in DORMANT. */
typedef enum {
    RX_DORMANT = 0,
    RX_READY,
    RX_BLOCKED_AUTHORITY,
    RX_BLOCKED_RESOURCE,
    RX_RUNNING,
    RX_PUBLISHING,
    RX_COMMITTED,
    RX_INVALIDATED,
    RX_FAILED,
    RX_CANCELLED,
    RX_CONFLICT,
    RX_REJECTED,
    RX_STATE_COUNT
} RxState;

/* Causal crumb kinds. */
typedef enum {
    RX_CRUMB_CREATE = 1,
    RX_CRUMB_EXTERNAL,
    RX_CRUMB_COMMIT,
    RX_CRUMB_INVALIDATED,
    RX_CRUMB_BLOCKED_AUTHORITY,
    RX_CRUMB_REJECTED,
    RX_CRUMB_FAILED,
    RX_CRUMB_NOOP,             /* ran, proposed no change: fixed point */
    RX_CRUMB_RETIRE
} RxCrumbKind;

/* Publication errors. */
#define RX_OK               0
#define RX_ERR_ARG         -1
#define RX_ERR_FULL        -2
#define RX_ERR_STALE_GEN   -3
#define RX_ERR_AUTHORITY   -4
#define RX_ERR_WRITE_SET   -5
#define RX_ERR_NOT_FOUND   -6
#define RX_ERR_TIMEOUT     -7

typedef struct { uint32_t id; uint32_t generation; } RxObjRef;

typedef struct {
    RxObjRef obj;
    uint64_t mask;              /* fields of interest */
} RxDep;

typedef struct {
    RxCapRef ref;
    uint64_t resource;
    uint32_t rights;
} RxCapNeed;

typedef struct {
    RxObjRef obj;
    uint32_t field;
    uint64_t value;
} RxMutation;

/* Versioned snapshot of one read dependency. */
typedef struct {
    RxObjRef obj;
    uint64_t mask;
    uint64_t version;
    uint64_t field[RX_MAX_FIELDS];
    uint64_t field_version[RX_MAX_FIELDS];
    uint64_t field_writer[RX_MAX_FIELDS];   /* causal id of the last writer */
} RxSnapshotDep;

typedef struct RxCtx {
    uint32_t n_in;
    RxSnapshotDep in[RX_MAX_DEPS];
    uint32_t n_out;
    RxMutation out[RX_MAX_MUTATIONS];
    void *user;
    uint32_t worker;
} RxCtx;

/* Pure transformation: read ctx->in, append proposals to ctx->out.
 * Return 0 to publish (possibly nothing), negative to fail. */
typedef int (*RxFn)(RxCtx *ctx);

typedef struct {
    const char *name;
    uint32_t faculty;
    uint32_t subject;           /* principal the capabilities must be bound to */
    uint32_t priority;
    uint32_t n_triggers;
    RxDep triggers[RX_MAX_DEPS];
    uint32_t n_reads;           /* triggers are read implicitly */
    RxDep reads[RX_MAX_DEPS];
    uint32_t n_writes;
    RxDep writes[RX_MAX_WRITES];
    uint32_t n_caps;
    RxCapNeed caps[RX_MAX_CAPS];
    RxFn fn;
    void *user;
} RxReactionDesc;

typedef struct {
    uint32_t id;
    uint32_t generation;
    uint32_t type;
    bool live;
    RxPersist persist;
    uint64_t resource;          /* capability resource that guards this object */
    uint64_t version;
    uint64_t field[RX_MAX_FIELDS];
    uint64_t field_version[RX_MAX_FIELDS];
    uint64_t field_writer[RX_MAX_FIELDS];
    uint8_t digest[32];         /* content identity of (type, fields) */
} RxObject;

typedef struct { uint32_t reaction; uint32_t generation; uint64_t mask; } RxSub;

typedef struct {
    uint64_t id;                /* 1-based, dense; 0 = none */
    RxCrumbKind kind;
    uint32_t reaction;          /* UINT32_MAX for external/create */
    uint32_t faculty;
    uint32_t worker;
    uint64_t wake_cause;        /* crumb that last woke this activation */
    uint64_t coalesced_wakes;   /* extra wakes folded into this activation */
    uint32_t n_inputs;
    struct { RxObjRef obj; uint64_t version; uint64_t mask; } inputs[RX_MAX_DEPS];
    uint32_t n_caps;
    RxCapRef caps[RX_MAX_CAPS];
    uint32_t n_outputs;
    struct { RxObjRef obj; uint64_t version; uint64_t mask; } outputs[RX_MAX_WRITES];
    uint32_t n_parents;
    uint64_t parents[RX_MAX_PARENTS];
    uint64_t t_start_ns;
    uint64_t t_end_ns;
    int32_t reason;
    uint8_t digest[32];         /* SHA-256 over canonical crumb bytes + parent digests */
} RxCrumb;

typedef struct {
    RxReactionDesc desc;
    RxState state;
    bool rearm;                 /* woken while running: go READY again after */
    uint64_t wake_cause;
    uint64_t coalesced;
    uint64_t activations;
    uint64_t commits;
} RxReaction;

typedef struct {
    uint64_t transitions[RX_STATE_COUNT][RX_STATE_COUNT];
    uint64_t illegal_transitions;
    uint64_t wakes;
    uint64_t coalesced_wakes;
    uint64_t subscriptions_checked;
    uint64_t commits;
    uint64_t invalidations;
    uint64_t blocked_authority;
    uint64_t rejected;
    uint64_t failed;
    uint64_t noops;
    uint64_t crumb_overflow;
} RxStats;

typedef struct RxWorld {
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    pthread_cond_t idle_cv;

    RxCapRoot *root;
    uint32_t external_subject;

    RxObject objects[RX_MAX_OBJECTS];
    RxSub *subs[RX_MAX_OBJECTS];
    uint32_t n_subs[RX_MAX_OBJECTS];

    RxReaction reactions[RX_MAX_REACTIONS];
    uint32_t n_reactions;

    /* Physical admission: bounded per-priority FIFO rings. */
    uint32_t ready_q[RX_PRIORITY_CLASSES][RX_MAX_REACTIONS];
    uint32_t ready_head[RX_PRIORITY_CLASSES];
    uint32_t ready_len[RX_PRIORITY_CLASSES];
    uint32_t in_flight;         /* READY + RUNNING + PUBLISHING */

    RxCrumb *crumbs;
    uint64_t n_crumbs;
    uint64_t crumb_cap;

    pthread_t workers[RX_MAX_WORKERS];
    uint32_t n_workers;
    bool stopping;

    RxStats stats;
} RxWorld;

int  rx_world_init(RxWorld *w, RxCapRoot *root, uint32_t n_workers, uint64_t crumb_cap);
void rx_world_destroy(RxWorld *w);

/* Object lifecycle. Creation writes a CREATE crumb. Retiring bumps the slot
 * generation, so every outstanding RxObjRef to it becomes stale. */
int  rx_world_create(RxWorld *w, uint32_t type, RxPersist persist, uint64_t resource,
                     const uint64_t init[RX_MAX_FIELDS], RxObjRef *out);
int  rx_world_retire(RxWorld *w, RxObjRef ref);

/* Register a reaction; subscriptions go into the dependency index. */
int  rx_world_add_reaction(RxWorld *w, const RxReactionDesc *d, uint32_t *out_id);

/* A stimulus from outside the organism (sensor, human input). Requires a
 * capability for (external_subject, object resource, WRITE). Returns the
 * causal id of the EXTERNAL crumb, or a negative error. */
int64_t rx_world_publish_external(RxWorld *w, RxCapRef cap,
                                  const RxMutation *muts, uint32_t n);

/* Wait until no reaction is READY/RUNNING/PUBLISHING. Observation only. */
int  rx_world_wait_quiescent(RxWorld *w, int timeout_ms);

/* Read-only inspection (takes the world lock). */
int  rx_world_read(RxWorld *w, RxObjRef ref, RxObject *out);
const RxCrumb *rx_world_crumb(const RxWorld *w, uint64_t id);
/* Causal id of the publication that last wrote obj.field. */
uint64_t rx_world_explain(RxWorld *w, RxObjRef ref, uint32_t field);
/* Recompute every crumb digest and check parent links; 0 on success. */
int  rx_world_verify_crumbs(RxWorld *w, uint64_t *out_checked);
/* SHA-256 over all live objects' (id, generation, type, fields). */
void rx_world_digest(RxWorld *w, uint8_t out[32]);

bool rx_state_transition_legal(RxState from, RxState to);
const char *rx_state_name(RxState s);
const char *rx_crumb_kind_name(RxCrumbKind k);

#endif /* RX_WORLD_H */
