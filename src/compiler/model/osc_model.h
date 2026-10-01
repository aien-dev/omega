/*
 * osc_model.h -- OSC-0B exit gate: the II.11 executable reference model of the
 * Omega Systems Core memory rules (OMEGA_SYSTEMS_CORE_CODE_AUDIT.md II.11).
 *
 * A deterministic state machine over objects, borrows, regions, pool slots
 * with u64 generations, live handles with rights, device cells (publication),
 * Quiesced tokens and SemanticIds. It is a reference oracle, not compiler
 * code. Fixed capacity, no malloc, no globals, no I/O.
 *
 * ---------------------------------------------------------------- EVENT API
 * Fill an OscModelEvent (zero it first; unused fields stay 0) and call
 * osc_model_step(). All ids are small integers 1..CAP; 0 means "none". An id
 * is created exactly once (single assignment); re-creating a used id, or
 * naming an id never created, is OSC_REJ_MALFORMED; an id > CAP is
 * OSC_REJ_CAPACITY. A rejected event leaves the model state unchanged.
 *
 * The OSC-1 front-end subset (OSC-1-DESIGN 3.1):
 *   OSC_EV_ALLOC        obj, region (0 = unique own<T>, else an open region)
 *   OSC_EV_MOVE         obj -> obj2 (fresh new owner id) or obj2 = 0 (consumed,
 *                       e.g. passed to an own parameter)
 *   OSC_EV_BORROW_SHARED obj, borrow (fresh), via (0 = borrow the owner
 *                       directly, else reborrow through live borrow `via`)
 *   OSC_EV_BORROW_MUT   obj, borrow (fresh), via (as above; `&mut r` = via r)
 *   OSC_EV_END_BORROW   borrow (borrow binding leaves scope / call returns)
 *   OSC_EV_USE_READ     obj, via (0 = direct use of the owner)
 *   OSC_EV_USE_WRITE    obj, via (0 = direct store into the owner)
 *   OSC_EV_RELEASE      obj (scope-end destruction of an unmoved owner)
 *   OSC_EV_REGION_OPEN / OSC_EV_REGION_DESTROY   region
 * Further events (runtime, handles, publication):
 *   OSC_EV_SLOT_ALLOC   slot, handle (fresh, or 0 = mint none), rights
 *   OSC_EV_SLOT_FREE    handle; or handle = 0 with explicit slot + gen
 *   OSC_EV_HANDLE_DERIVE handle (fresh) from handle2 (parent), rights
 *   OSC_EV_HANDLE_USE   handle, rights (the rights the use needs)
 *   OSC_EV_PERSIST      handle -> semid (fresh SemanticId; resolves live object)
 *   OSC_EV_DURABLE_WRITE vkind (OSC_VK_*), id: write a value to a durable encoder
 *   OSC_EV_CELL_WRITE   cell (first write creates the cell; starts a round)
 *   OSC_EV_CELL_PUBLISH cell, order (must be OSC_ORD_RELEASE)
 *   OSC_EV_CELL_OBSERVE cell, order (must be OSC_ORD_ACQUIRE)
 *   OSC_EV_CELL_READ    cell (consumer reads device data)
 *   OSC_EV_QUIESCE      token (fresh own<Quiesced>), cell (must be observed)
 *   OSC_EV_RECLAIM      cell, token (0 = no token); consumes the token
 *
 * ------------------------------------------------- CLASSIFICATION CHOICES
 * Checks run in a fixed order: id range (CAPACITY), id existence/freshness
 * (MALFORMED), then the rules below in the order listed per event in
 * osc_model.c. Choices where the II.11 list leaves room:
 *  - Moving, or storing directly into, an owner while any borrow of it is live,
 *    reading it directly while a &mut is live, a second &mut, & with &mut, a
 *    &mut reborrow while the parent already has a live child, and using a &mut
 *    parent while its &mut child is live: MUTABLE_ALIAS (OSC-1-DESIGN 3.1).
 *  - Releasing an owner while a borrow of it is live, or ending a borrow while
 *    a reborrow of it is live: BORROW_OUTLIVES_OWNER.
 *  - Destroying a region while a borrow of an object in it is live:
 *    ARENA_ESCAPE (the reference outlives its region).
 *  - Writing through a shared borrow, or taking &mut through a shared borrow:
 *    FORGED_RIGHTS (a shared borrow carries read rights only; II.12
 *    "mutability is authority"). The compiler's READ_ONLY_BORROW maps here.
 *  - Any use of a MOVED object (including release): USE_AFTER_MOVE.
 *  - Use of a RELEASED object, of an ended borrow, of a reclaimed cell, or an
 *    alloc into a destroyed region: USE_AFTER_RELEASE.
 *  - Second release of an object / end of a borrow / destroy of a region /
 *    reclaim of a cell: DOUBLE_RELEASE.
 *  - Any handle (or explicit slot+gen) whose generation is not the slot's live
 *    generation, including a second SLOT_FREE and use after retirement:
 *    STALE_GENERATION. Stale is checked before rights.
 *  - SLOT_ALLOC of a retired slot: GENERATION_WRAP. A slot whose generation
 *    equals gen_max is retired on free, never wrapped (OSC-0 decision 1).
 *  - DURABLE_WRITE of a handle or a borrow (any non-durable live reference):
 *    LIVE_HANDLE_DURABLE, regardless of the reference's liveness.
 *  - CELL_OBSERVE with a non-acquire order, CELL_READ before an acquire
 *    observe of the current round: READ_BEFORE_OBSERVE.
 *  - CELL_PUBLISH with a non-release order: PUBLISH_WITHOUT_RELEASE (checked
 *    before the cell's state).
 *  - RECLAIM with no token, a consumed token, or a token for another cell:
 *    RECLAIM_WITHOUT_QUIESCED.
 *  - Out-of-protocol transitions outside the II.11 list (publish of an
 *    unwritten cell, observe of an unpublished cell, QUIESCE of an unobserved
 *    cell, SLOT_ALLOC of a live slot, unknown event kind): OSC_REJ_PROTOCOL.
 */
#ifndef OSC_MODEL_H
#define OSC_MODEL_H

#include <stdint.h>

#define OSC_MODEL_MAX_OBJECTS  64
#define OSC_MODEL_MAX_BORROWS  64
#define OSC_MODEL_MAX_REGIONS  16
#define OSC_MODEL_MAX_SLOTS    16
#define OSC_MODEL_MAX_HANDLES  64
#define OSC_MODEL_MAX_CELLS    16
#define OSC_MODEL_MAX_TOKENS   32
#define OSC_MODEL_MAX_SEMIDS   32

typedef enum {
    OSC_EV_ALLOC = 1,
    OSC_EV_MOVE,
    OSC_EV_BORROW_SHARED,
    OSC_EV_BORROW_MUT,
    OSC_EV_END_BORROW,
    OSC_EV_USE_READ,
    OSC_EV_USE_WRITE,
    OSC_EV_RELEASE,
    OSC_EV_REGION_OPEN,
    OSC_EV_REGION_DESTROY,
    OSC_EV_SLOT_ALLOC,
    OSC_EV_SLOT_FREE,
    OSC_EV_HANDLE_DERIVE,
    OSC_EV_HANDLE_USE,
    OSC_EV_PERSIST,
    OSC_EV_DURABLE_WRITE,
    OSC_EV_CELL_WRITE,
    OSC_EV_CELL_PUBLISH,
    OSC_EV_CELL_OBSERVE,
    OSC_EV_CELL_READ,
    OSC_EV_QUIESCE,
    OSC_EV_RECLAIM,
    OSC_EV__COUNT
} OscModelEventKind;

/* Memory order on publication events. */
typedef enum { OSC_ORD_RELAXED = 0, OSC_ORD_RELEASE = 1, OSC_ORD_ACQUIRE = 2 } OscModelOrder;

/* Kind of value handed to a durable encoder (OSC_EV_DURABLE_WRITE). */
typedef enum {
    OSC_VK_OBJECT = 1,  /* plain value read from an owner: durable           */
    OSC_VK_SEMID,       /* SemanticId from OSC_EV_PERSIST: durable             */
    OSC_VK_HANDLE,      /* live Handle<T>: non-durable                         */
    OSC_VK_BORROW       /* &T / &mut T: non-durable                            */
} OscModelValueKind;

typedef struct {
    uint32_t kind;     /* OscModelEventKind */
    uint32_t obj;      /* object id */
    uint32_t obj2;     /* MOVE destination (0 = consumed) */
    uint32_t borrow;   /* borrow id (created by BORROW_*, named by END_BORROW) */
    uint32_t via;      /* borrow used for access / reborrow parent; 0 = direct */
    uint32_t region;   /* region id */
    uint32_t slot;     /* pool slot id */
    uint32_t handle;   /* handle id */
    uint32_t handle2;  /* parent handle for HANDLE_DERIVE */
    uint32_t cell;     /* device cell id */
    uint32_t token;    /* Quiesced token id */
    uint32_t semid;    /* SemanticId id */
    uint32_t order;    /* OscModelOrder */
    uint32_t vkind;    /* OscModelValueKind for DURABLE_WRITE */
    uint32_t id;       /* value id for DURABLE_WRITE (obj/semid/handle/borrow) */
    uint64_t gen;      /* explicit generation (SLOT_FREE with handle = 0) */
    uint64_t rights;   /* rights mask */
} OscModelEvent;

/* Verdict. 1..13 map 1:1 to the II.11 named invalid transitions, in order. */
typedef enum {
    OSC_REJ_NONE = 0,              /* accepted */
    OSC_REJ_USE_AFTER_MOVE = 1,
    OSC_REJ_USE_AFTER_RELEASE,
    OSC_REJ_DOUBLE_RELEASE,
    OSC_REJ_STALE_GENERATION,
    OSC_REJ_GENERATION_WRAP,
    OSC_REJ_MUTABLE_ALIAS,
    OSC_REJ_BORROW_OUTLIVES_OWNER,
    OSC_REJ_ARENA_ESCAPE,
    OSC_REJ_RECLAIM_WITHOUT_QUIESCED,
    OSC_REJ_READ_BEFORE_OBSERVE,
    OSC_REJ_PUBLISH_WITHOUT_RELEASE,
    OSC_REJ_FORGED_RIGHTS,
    OSC_REJ_LIVE_HANDLE_DURABLE,   /* = 13 */
    OSC_REJ_MALFORMED,             /* id never created / reused / bad field */
    OSC_REJ_CAPACITY,              /* id beyond a fixed capacity */
    OSC_REJ_PROTOCOL,              /* out-of-protocol, not in the II.11 list */
    OSC_REJ__COUNT
} OscModelReject;

#define OSC_MODEL_NAMED_COUNT 13

typedef enum { OSC_MODEL_ACCEPT = 0, OSC_MODEL_REJECT = 1 } OscModelVerdict;

typedef struct { uint8_t state, n_mut; uint16_t n_shared; uint32_t region; } OscMObj;
typedef struct { uint8_t state, mut; uint16_t live_kids_shared, live_kids_mut; uint32_t obj, parent; } OscMBorrow;
typedef struct { uint8_t state; uint64_t gen; } OscMSlot;
typedef struct { uint8_t used; uint32_t slot; uint64_t gen, rights; } OscMHandle;
typedef struct { uint8_t state; } OscMCell;
typedef struct { uint8_t state; uint32_t cell; } OscMToken;

typedef struct {
    uint64_t gen_base, gen_max;
    OscMObj    obj[OSC_MODEL_MAX_OBJECTS + 1];
    OscMBorrow bor[OSC_MODEL_MAX_BORROWS + 1];
    uint8_t    region[OSC_MODEL_MAX_REGIONS + 1];
    OscMSlot   slot[OSC_MODEL_MAX_SLOTS + 1];
    OscMHandle hnd[OSC_MODEL_MAX_HANDLES + 1];
    OscMCell   cell[OSC_MODEL_MAX_CELLS + 1];
    OscMToken  tok[OSC_MODEL_MAX_TOKENS + 1];
    uint8_t    semid[OSC_MODEL_MAX_SEMIDS + 1];
    uint64_t   accepted, rejected;
} OscModel;

/* Every slot starts free(gen_base); a slot freed at generation gen_max is
 * retired. Production: gen_base = 0, gen_max = UINT64_MAX. Tests use small
 * gen_max (or gen_base near UINT64_MAX) so the retire/wrap path is reachable.
 * gen_base > gen_max is clamped to gen_max. */
void osc_model_init(OscModel *m, uint64_t gen_base, uint64_t gen_max);

/* Apply one event. Returns OSC_MODEL_ACCEPT (and *out_reject = OSC_REJ_NONE)
 * or OSC_MODEL_REJECT with the typed reason; on reject the state is
 * unchanged. out_reject may be NULL. Deterministic. */
OscModelVerdict osc_model_step(OscModel *m, const OscModelEvent *ev, OscModelReject *out_reject);

/* "use-after-move", ..., "malformed", "capacity", "protocol"; "accept" for 0;
 * "?" for out-of-range. */
const char *osc_model_reject_name(OscModelReject r);
const char *osc_model_event_name(uint32_t kind);

#endif
