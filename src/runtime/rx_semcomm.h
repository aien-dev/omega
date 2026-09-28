/*
 * rx_semcomm.h -- semantic communication pruning for J-space branches and
 * swarms (OMEGA_SEMANTIC_COMMUNICATION).
 *
 * A branch or cognitive worker does not receive another worker's history or
 * the whole World. It declares an InformationNeed: what operation it will
 * perform, on which types and objects, over which part of time, with what
 * evidence and what uncertainty. Omega answers with a SemanticProjection:
 * the object references, fields, derived values and evidence references that
 * operation needs, and nothing else.
 *
 *   InformationNeed + World + receiver's capabilities
 *        -> rx_sem_project   select, check authority, derive
 *        -> rx_sem_encode    canonical bytes (one encoder for every mode)
 *        -> rx_sem_delta     after the first delivery: only what changed
 *
 * Authority. Every object in a projection was validated for READ through the
 * world's authority view at the time of projection, for the receiver's own
 * principal. Fields a type marks private need READ on the object's private
 * resource as well. A projection carries values and references. It carries
 * no capability, so receiving one gives no write authority: a write still
 * needs the writer's own capability, checked by the engine at start and at
 * publication. This file takes no admin handle and never mints; the build
 * checks its object file for that.
 *
 * Uncertainty. Omega does not estimate uncertainty. A type may declare one
 * field as its producer's uncertainty (parts per million). A need may exclude
 * objects above a bound and may ask for that field to be delivered.
 *
 * Time. The time of a field is the causal id of the crumb that last wrote it
 * (rx_world_explain). A time range selects fields by that id.
 *
 * Evidence. An evidence reference is that writer crumb id, and on request
 * the crumb's content digest, so a receiver can check it against the World.
 */
#ifndef RX_SEMCOMM_H
#define RX_SEMCOMM_H

#include "rx_world.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_MAX_TYPES      8u
#define SC_MAX_RELEVANT   64u
#define SC_MAX_CAPS       32u
#define SC_MAX_SCHEMA     16u
#define SC_MAX_DERIVED    4u
#define SC_MAX_FIELDS     (RX_MAX_OBJECTS * RX_MAX_FIELDS)

/* Private-field resource of an object guarded by `resource`. */
#define SC_PRIVATE_BIT    (1ull << 62)
static inline uint64_t rx_sem_private_resource(uint64_t resource) { return resource | SC_PRIVATE_BIT; }

/* What the receiver will do with the state. It decides which fields travel
 * and whether Omega sends a derived value instead of the inputs. */
enum {
    SC_OP_INSPECT = 1,   /* needs the listed fields of every selected object */
    SC_OP_SUM,           /* needs the sum of one field over the selected objects */
    SC_OP_MAX            /* needs the largest value of one field and which object holds it */
};

enum { SC_EV_NONE = 0, SC_EV_WRITER = 1, SC_EV_DIGEST = 2 };

/* Why an object or a field stopped being part of a receiver's projection. */
enum {
    SC_INV_RETIRED = 1,     /* the object's generation moved */
    SC_INV_AUTHORITY,       /* the receiver no longer holds READ */
    SC_INV_UNCERTAIN,       /* its declared uncertainty now exceeds the bound */
    SC_INV_SCOPE            /* out of the need's time range or type set */
};

/* Message kinds. */
enum { SC_MSG_FULL_STATE = 1, SC_MSG_PROJECTION = 2, SC_MSG_DELTA = 3 };

#define SC_OK            0
#define SC_E_ARG        -1
#define SC_E_FULL       -2
#define SC_E_DECODE     -3

/* Per-type facts Omega uses. Not a claim about meaning, only about layout. */
typedef struct {
    uint32_t type;
    uint64_t private_mask;      /* fields that need READ on the private resource */
    int32_t uncertainty_field;  /* -1 = this type declares no uncertainty */
    bool conclusion;            /* new objects of this type travel as conclusions */
} RxSemType;

typedef struct {
    uint32_t n;
    RxSemType t[SC_MAX_SCHEMA];
} RxSemSchema;

/* The receiver's own capabilities. Omega validates them; it does not add any. */
typedef struct {
    uint32_t subject;
    uint32_t n;
    struct { RxCapRef ref; uint64_t resource; uint32_t rights; } cap[SC_MAX_CAPS];
} RxSemCaps;

typedef struct {
    uint32_t kind;              /* SC_OP_* */
    uint64_t fields;            /* INSPECT: field mask */
    uint32_t field;             /* SUM, MAX: the field */
} RxSemOperation;

typedef struct {
    uint32_t receiver;                          /* principal; must equal caps->subject */
    RxSemOperation operation;
    uint32_t n_types;                           /* 0 = any type */
    uint32_t required_types[SC_MAX_TYPES];
    uint32_t n_objects;                         /* 0 = every object of the required types */
    RxObjRef relevant_objects[SC_MAX_RELEVANT];
    struct { uint64_t from_crumb, to_crumb; } relevant_time_range;  /* to 0 = open */
    uint32_t evidence_requirement;              /* SC_EV_* */
    struct { uint32_t max_ppm; bool deliver; } uncertainty_requirement;  /* max 0 = any */
} RxInformationNeed;

typedef struct {
    RxObjRef ref;
    uint32_t type;
    uint64_t resource;
    uint64_t mask;              /* fields delivered for this object */
    bool conclusion;
} RxSemObject;

typedef struct {
    uint16_t obj;               /* index into object_refs */
    uint8_t field;
    uint64_t value;
    uint64_t version;
} RxSemField;

typedef struct {
    uint32_t op;                /* SC_OP_SUM or SC_OP_MAX */
    uint32_t field;
    uint64_t value;
    RxObjRef arg;               /* MAX: the object holding it; {0,0} for SUM */
    uint32_t n_inputs;
} RxSemDerived;

typedef struct {
    uint16_t obj;
    uint8_t field;
    uint64_t crumb;
    uint8_t digest[32];         /* only filled for SC_EV_DIGEST */
} RxSemEvidence;

typedef struct {
    uint32_t n_objects;
    RxSemObject object_refs[RX_MAX_OBJECTS];
    uint32_t n_fields;
    RxSemField fields[SC_MAX_FIELDS];
    uint32_t n_derived;
    RxSemDerived derived_values[SC_MAX_DERIVED];
    uint32_t n_evidence;
    RxSemEvidence evidence_refs[SC_MAX_FIELDS];
    uint32_t evidence_mode;
    /* Observability, not delivered. */
    uint32_t withheld_authority;    /* objects in scope the receiver may not read */
    uint32_t withheld_private;      /* private fields withheld */
    uint32_t excluded_uncertain;
    uint32_t validations;           /* authority checks performed */
} RxSemanticProjection;

/* Compute the projection of the current World for `need`. `caps` must belong
 * to need->receiver. Takes the world lock once; reads only. */
int rx_sem_project(RxWorld *w, const RxSemSchema *schema, const RxInformationNeed *need,
                   const RxSemCaps *caps, RxSemanticProjection *out);

/* ---- canonical encoding ----
 * Little-endian, fixed record layouts, objects in id order. The same records
 * serve full-state broadcast, projections and deltas, so byte counts compare. */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} RxSemBuf;

void   rx_sem_buf_reset(RxSemBuf *b);
void   rx_sem_buf_free(RxSemBuf *b);

/* Everything: every live object, every field with its version and writer.
 * This is the broadcast baseline. */
int rx_sem_encode_full_state(RxWorld *w, uint32_t receiver, uint64_t seq, RxSemBuf *out);
int rx_sem_encode_projection(const RxSemanticProjection *p, uint32_t receiver, uint64_t seq,
                             RxSemBuf *out);

/* ---- delta delivery ----
 * The sender keeps, per receiver, what it last delivered. A delta carries
 * changed relevant fields, new evidence, invalidations, new objects (tagged
 * as conclusions where the type says so) and changed derived values. The
 * cursor then equals the new projection. */
typedef struct {
    bool have;                  /* at least one delivery happened */
    bool present[RX_MAX_OBJECTS];
    uint32_t gen[RX_MAX_OBJECTS];
    uint64_t mask[RX_MAX_OBJECTS];
    uint64_t version[RX_MAX_OBJECTS][RX_MAX_FIELDS];
    uint64_t crumb[RX_MAX_OBJECTS][RX_MAX_FIELDS];
    uint32_t n_derived;
    RxSemDerived derived[SC_MAX_DERIVED];
} RxSemCursor;

typedef struct {
    uint32_t changed_fields, new_evidence, invalidated_objects, invalidated_fields;
    uint32_t new_objects, new_conclusions, derived_changed;
} RxSemDeltaStats;

/* Encode what the receiver lacks relative to `cur`, then advance `cur` to
 * `p`. With no prior delivery this is the whole projection. `reason_of`
 * explains an object that left (may be null: SC_INV_SCOPE). */
typedef uint32_t (*RxSemReasonFn)(void *ctx, RxObjRef gone);
int rx_sem_delta(RxSemCursor *cur, const RxSemanticProjection *p, uint32_t receiver,
                 uint64_t seq, RxSemReasonFn reason_of, void *reason_ctx, RxSemBuf *out,
                 RxSemDeltaStats *st);

/* Bytes of sender-side state one cursor represents (for memory accounting). */
size_t rx_sem_cursor_bytes(const RxSemCursor *cur);

/* ---- receiver ----
 * What a receiver holds after decoding. It applies full states, projections
 * and deltas alike. */
typedef struct {
    bool present[RX_MAX_OBJECTS];
    uint32_t gen[RX_MAX_OBJECTS];
    uint32_t type[RX_MAX_OBJECTS];
    uint64_t resource[RX_MAX_OBJECTS];
    bool conclusion[RX_MAX_OBJECTS];
    uint64_t mask[RX_MAX_OBJECTS];
    uint64_t value[RX_MAX_OBJECTS][RX_MAX_FIELDS];
    uint64_t version[RX_MAX_OBJECTS][RX_MAX_FIELDS];
    uint64_t crumb[RX_MAX_OBJECTS][RX_MAX_FIELDS];
    uint64_t ev_mask[RX_MAX_OBJECTS];               /* fields with evidence held */
    uint8_t (*digest)[RX_MAX_FIELDS][32];           /* allocated on first digest record */
    uint32_t n_derived;
    RxSemDerived derived[SC_MAX_DERIVED];
    uint64_t last_seq;
    uint32_t invalidations_seen;
    uint32_t conclusions_seen;
} RxSemView;

void rx_sem_view_init(RxSemView *v);
void rx_sem_view_free(RxSemView *v);
/* Decode and apply one message. A full state or projection replaces the view;
 * a delta edits it. */
int rx_sem_view_apply(RxSemView *v, const uint8_t *msg, size_t len);
/* Bytes of state the view actually holds (objects, fields, evidence, derived). */
size_t rx_sem_view_bytes(const RxSemView *v);
/* SHA-256 over the held content in canonical order. A view rebuilt from
 * deltas and a view built from the fresh projection hash the same. */
void rx_sem_view_digest(const RxSemView *v, uint8_t out[32]);

#endif /* RX_SEMCOMM_H */
