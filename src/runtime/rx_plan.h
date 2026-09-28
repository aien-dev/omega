/*
 * rx_plan.h -- Omega plan IR and verified plan cache (OMEGA_PLAN_REUSE).
 *
 * A plan is semantic computation: an action graph (rx_graph.h) whose World
 * objects are slots, together with the conditions under which it is valid
 * and the evidence it must leave. It is not prose and not a hidden chain of
 * thought.
 *
 *   goal object -> goal shape -> candidate templates -> applicability
 *     -> bind slots -> capability / resource conditions -> execute -> verify
 *
 * When nothing applies, AIEN plans (a domain planner, rx_plan_arrange.h);
 * the plan is executed and verified, and its generalisation is stored as a
 * CANDIDATE template. A CANDIDATE reuse first runs the graph's sequential
 * reference on a copy of the World; a verified reuse promotes it to
 * VERIFIED, which skips the reference. A reuse that does not verify marks it
 * STALE and counts as a false applicability.
 *
 * Plan reuse is never blind replay. Every retrieval is checked against the
 * goal's constraints, the World state the plan needs, the principal's
 * authority (validated through the World's authority view, never minted),
 * the resources the body offers now, the environment the plan assumes, and
 * the World and cognitive generations it was verified under. Each accept or
 * refusal is published to a World decision object, so it leaves a crumb.
 *
 * Identity. semantic_id is SHA-256 over canonical bytes of the semantic
 * fields only (predicate lists sorted, the graph by its builder-order
 * independent digest). Ancestry, verification status and counters are not
 * semantics. A realization (one binding in one World, the capabilities
 * used, the compiled graph, the World generation and core class) has its
 * own realization_id; many realizations share one semantic id.
 *
 * The store is host-side. It takes no admin handle and cannot mint.
 */
#ifndef RX_PLAN_H
#define RX_PLAN_H

#include "rx_graph.h"
#include "rx_world.h"

#include <stdint.h>

#define PL_MAX_SLOTS      8u   /* AgGoal carries 8 World arguments */
#define PL_MAX_PREDS      48u
#define PL_MAX_TEMPLATES  32u

/* Object types reserved for plans (0x5B01..0x5B0F). */
enum {
    PL_OT_UNIT = 0x5B01u,       /* arrangement domain unit (rx_plan_arrange.h) */
    PL_OT_WORLDGEN,             /* 0 World generation */
    PL_OT_COGNITION,            /* 0 cognitive generation, 1 planner model id */
    PL_OT_STATION,              /* environment: 0 arm ready */
    PL_OT_GOAL,                 /* 0 seq, 1 kind, 2 max steps, 3 energy budget, 4 facts ref */
    PL_OT_DECISION,             /* see rx_plan_decide */
    PL_OT_INDEX,                /* see rx_plan_publish_index */
    PL_OT_GOALFACTS             /* eight packed goal facts */
};

/* Named environment objects, in PlEnv.obj order. */
enum { PL_ENV_GOAL = 0, PL_ENV_WORLDGEN, PL_ENV_COGNITION, PL_ENV_STATION, PL_ENV_FACTS,
       PL_ENV_COUNT };

/* Packed reference stored in a field: bit 63 set, generation, id. 0 = none. */
static inline uint64_t pl_ref_pack(RxObjRef r) {
    return (1ull << 63) | ((uint64_t)r.generation << 32) | r.id;
}
static inline RxObjRef pl_ref_unpack(uint64_t v) {
    return (RxObjRef){ (uint32_t)(v & 0xffffffffu), (uint32_t)((v >> 32) & 0x7fffffffu) };
}

typedef enum {
    PL_P_EQ_CONST = 1,  /* slot.field == value */
    PL_P_EQ_SLOT,       /* slot.field == packed ref of slot2 */
    PL_P_NOT_SLOT,      /* slot.field != 0 and names no slot */
    PL_P_UNREFERENCED,  /* no live object of `type` outside the slots has field == ref(slot) */
    PL_P_SELF_REF,      /* slot.field == packed ref of slot itself */
    PL_P_LIVE,          /* slot is live at its bound generation and of `type` */
    PL_P_ENV_EQ,        /* env[slot].field == value */
    PL_P_ENV_GE         /* env[slot].field >= value */
} PlPredKind;

typedef struct {
    uint32_t kind, slot, field, slot2, type;
    uint64_t value;
} PlPred;

typedef struct {
    uint32_t n;
    PlPred p[PL_MAX_PREDS];
} PlPredList;

enum { PL_ROLE_GOAL = 1, PL_ROLE_INVOLVED };

/* How an involved slot is found in a World: the unique live object of the
 * slot's type whose field `field` holds the packed ref of slot `from`. */
enum { PL_BIND_FROM_GOAL = 0, PL_BIND_RESTS_ON = 1 };

typedef struct {
    uint32_t type;
    uint32_t role;
    uint32_t rights;            /* capability rights the plan needs on this object */
    uint32_t bind_rule, bind_from, bind_field;
} PlSlot;

enum { PL_STATUS_CANDIDATE = 1, PL_STATUS_VERIFIED, PL_STATUS_STALE };
enum { PL_ORIGIN_SEARCH = 1, PL_ORIGIN_ADAPTED, PL_ORIGIN_REDERIVED };
enum { PL_GRAPH_RX_GRAPH = 1 };                         /* action_graph kind */
enum { PL_FAIL_GRAPH = 1u, PL_FAIL_INVARIANT = 2u, PL_FAIL_EVIDENCE = 4u, PL_FAIL_CRUMBS = 8u };

/* Applicability axes, in check order. The first failing one is reported. */
typedef enum {
    PL_AX_OK = 0,
    PL_AX_SHAPE,
    PL_AX_BINDING,
    PL_AX_GOAL,
    PL_AX_WORLD_GEN,
    PL_AX_COG_GEN,
    PL_AX_ENVIRONMENT,
    PL_AX_STATE,
    PL_AX_AUTHORITY,
    PL_AX_RESOURCES,
    PL_AX_STALE,            /* template is STALE */
    PL_AX_COUNT
} PlAxis;

typedef struct {
    uint32_t origin;            /* PL_ORIGIN_* */
    uint32_t has_parent;
    uint8_t parent[32];         /* semantic id of the template this came from */
    uint64_t goal_seq;          /* goal that produced it */
    uint64_t cog_gen;           /* cognitive generation that produced it */
} PlAncestry;

typedef struct {
    /* ---- semantic (hashed) ---- */
    uint32_t goal_kind;
    uint8_t goal_shape[32];
    uint32_t n_goal_slots;      /* slots 0..n-1 bound from the goal */
    uint32_t n_slots;
    PlSlot slots[PL_MAX_SLOTS];
    PlPredList pre;             /* over the goal / environment objects */
    PlPredList state;           /* required World state over slots */
    PlPredList env;             /* environment assumptions */
    PlPredList invariants;      /* before and after */
    PlPredList success;
    uint32_t fail_flags;        /* failure predicate */
    RxResourceNeed step_need;   /* per step */
    uint32_t n_steps;
    uint64_t energy_total;
    uint32_t n_evidence;        /* evidence nodes after compile */
    uint32_t graph_kind;
    uint8_t graph_digest[32];
    AgGraph graph;              /* the template: World objects are params 1..n_slots */
    /* ---- not semantic ---- */
    uint8_t semantic_id[32];
    PlAncestry ancestry;
    uint32_t status;
    uint64_t verified_world_gen, verified_cog_gen;
    uint64_t uses, successes, failures, promotions, rederivations;
    uint64_t refusals[PL_AX_COUNT];
} PlanTemplate;

/* Canonical semantic bytes. Returns length, or 0 when buf is too small. */
size_t rx_plan_encode(const PlanTemplate *t, uint8_t *buf, size_t cap);
/* Decode the semantic part (everything but the graph body, which is carried
 * by its digest). 0 on success. */
int    rx_plan_decode(const uint8_t *buf, size_t n, PlanTemplate *t);
/* Fill graph_digest (rx_graph_identify of the unbound template) and semantic_id. */
void   rx_plan_identify(PlanTemplate *t);
void   rx_plan_sort(PlanTemplate *t);  /* canonical order of every predicate list */

/* ---- World views ---- */

typedef struct {
    RxObjRef obj[PL_ENV_COUNT];
} PlEnv;

/* A consistent copy of every object slot, taken under the world lock. */
typedef struct {
    uint32_t n;
    struct { uint32_t type, generation; uint8_t live; uint64_t resource;
             uint64_t field[RX_MAX_FIELDS]; } o[RX_MAX_OBJECTS];
} PlView;

void rx_plan_view(RxWorld *w, PlView *v);

/* What cognition cost. */
typedef struct {
    uint64_t shape_ops;         /* goal-shape derivation steps */
    uint64_t candidates;        /* templates retrieved */
    uint64_t preds;             /* predicates evaluated */
    uint64_t bind_attempts;
    uint64_t expanded, generated, heuristic;   /* planner */
} PlCost;

static inline uint64_t pl_cost_ops(const PlCost *c) {
    return c->shape_ops + c->candidates + c->preds + c->bind_attempts + c->expanded +
           c->generated + c->heuristic;
}

/* ---- checks ---- */

/* 1 when the predicate holds. slots may be NULL for environment predicates. */
int rx_plan_pred(const PlPred *p, const PlView *v, const PlEnv *env, const RxObjRef *slots,
                 uint32_t n_slots);
/* Index of the first failing predicate, or -1 when all hold. */
int rx_plan_preds(const PlPredList *l, const PlView *v, const PlEnv *env, const RxObjRef *slots,
                  uint32_t n_slots, PlCost *cost);

/* Every applicability axis except shape (checked by retrieval), in PlAxis
 * order. *pred_out receives the failing predicate index (or slot) when
 * useful. The authority axis validates each needed capability through the
 * World's authority view; nothing is minted. */
PlAxis rx_plan_applicable(const PlanTemplate *t, RxWorld *w, const PlView *v, const PlEnv *env,
                          const RxObjRef *slots, const AgCapTable *caps, PlCost *cost,
                          int *pred_out);

/* ---- store ---- */

typedef struct {
    uint32_t n;
    PlanTemplate *t[PL_MAX_TEMPLATES];
    uint64_t stored, deduplicated;
} PlCache;

void rx_plan_cache_init(PlCache *c);
void rx_plan_cache_free(PlCache *c);
/* Store a verified first execution. When a template with the same semantic id
 * exists, its verification record is renewed (re-derivation) and no copy is
 * made. Returns the stored template. */
PlanTemplate *rx_plan_cache_store(PlCache *c, const PlanTemplate *t, uint64_t world_gen,
                                  uint64_t cog_gen);
PlanTemplate *rx_plan_cache_find(PlCache *c, const uint8_t semantic_id[32]);
/* Templates of this shape and kind, in store order. */
uint32_t rx_plan_cache_retrieve(PlCache *c, uint32_t goal_kind, const uint8_t shape[32],
                                PlanTemplate **out, uint32_t max, PlCost *cost);

/* ---- execution ---- */

typedef struct {
    RxCapRef cell_cap, run_cap, ext_run_cap;
    uint64_t cell_res, run_res;
    const AgSkillTable *skills;
    uint64_t core_class;        /* recorded in the realization */
    int reference_first;        /* CANDIDATE: run the sequential reference first */
} PlExecCtx;

/* Compiled graph and its lowering. Both must outlive the world's reactions. */
typedef struct { AgGraph g; AgLowered L; } PlRunStore;

typedef int (*PlLegalFn)(const PlView *v);     /* domain World legality */

typedef struct {
    int compile_verdict;
    uint32_t n_missing, n_resource_blocked;
    int reference_outcome;      /* AG_RUN_* when run, 0 when not */
    int outcome;                /* graph AG_RUN_* */
    uint32_t fail;              /* PL_FAIL_* that held */
    int success;                /* success predicate held and no failure flag */
    uint32_t nodes, evidence_required, evidence_present;
    uint64_t crumbs_before, crumbs_after, crumbs_checked;
    uint8_t realization_id[32];
    uint8_t compiled_digest[32];
    uint64_t compile_ns, run_ns, verify_ns, reference_ns;
} PlExecResult;

/* Compile the template with `slots` bound, optimise, lower into caller
 * storage rs, run once as run id
 * `run`, wait, collect, and verify. Checks nothing about applicability. */
int rx_plan_execute(const PlanTemplate *t, RxWorld *w, const PlEnv *env, const RxObjRef *slots,
                    const AgCapTable *caps, const PlExecCtx *x, uint64_t run, PlRunStore *rs,
                    PlLegalFn legal, PlExecResult *out);

/* After a reuse: success promotes CANDIDATE to VERIFIED; failure marks STALE. */
void rx_plan_record_use(PlanTemplate *t, int success);

/* ---- World records ---- */

/* Decision fields: 0 goal seq, 1 axis (0 = accepted), 2..3 semantic id words
 * 0..1 (0 when no candidate), 4 failing predicate + 1 (0 none), 5 decision
 * seq, 6 realization id word 0 (accepted and run), 7 status after. Published
 * as an outside publication; returns the crumb id or a negative error. */
int64_t rx_plan_decide(RxWorld *w, RxCapRef cap, RxObjRef decision, uint64_t goal_seq,
                       PlAxis axis, const PlanTemplate *t, int pred, uint64_t seq,
                       const uint8_t *realization_id);

/* Index fields: 0 templates stored, 1..2 semantic id words 0..1, 3 status,
 * 4 verified World generation, 5 uses, 6 successes, 7 failures. */
int64_t rx_plan_publish_index(RxWorld *w, RxCapRef cap, RxObjRef index, const PlCache *c,
                              const PlanTemplate *t);

uint64_t pl_word(const uint8_t id[32], uint32_t k);
const char *rx_plan_axis_name(PlAxis a);
const char *rx_plan_status_name(uint32_t s);

#endif /* RX_PLAN_H */
