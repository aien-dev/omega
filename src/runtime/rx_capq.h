/*
 * rx_capq.h -- Omega capability query IR (OMEGA_CAPABILITY_QUERY).
 *
 * AIEN says what it needs done. It does not need to know which Skill, tool,
 * device or machine can do it. It states a CapabilityNeed: the semantic
 * operation, the types it can supply and must get back, the effects it
 * accepts, the most authority it will use, where the work may run, its
 * latency and energy budgets, and how reliable and how well evidenced the
 * provider must be. Omega compiles the need into a plan of probes against
 * the sources that can realize that operation:
 *
 *   Capability Graph      native AIEN capabilities
 *   Skill Network         reusable procedures (Skill Net)
 *   MCP registry          MCP tools, where the operation allows them
 *   local physical        this machine's devices
 *   Fabric                capabilities other machines in the house advertise
 *
 * and runs it against an index. What comes back into cognition is a short,
 * fixed-size list of CapabilityCandidate records: ids and numbers, no
 * description text. The catalog's descriptions stay in the catalog. The
 * bytes AIEN receives are bounded by CQ_MAX_K, whatever the catalog size.
 *
 * Ranking. There is no relevance score. Each candidate carries its values on
 * explicit dimensions (cost, latency, energy, confidence, reliability,
 * whether its authority is already held, locality). The caller names the
 * dimensions that matter, in priority order, with a tolerance band on each.
 * Omega keeps the candidates no other candidate beats on every named
 * dimension (the Pareto front), then orders them by the caller's priorities:
 * on the first dimension it keeps everything within the band of the best,
 * then the next dimension decides among those, and so on. Ties end on the
 * ids, so the order is deterministic.
 *
 * Authority. A candidate states the authority it needs. The query compares
 * it with the need's ceiling (pure comparison), and may ask the world's
 * authority view whether the principal already holds it (a validation, never
 * a mint). This file has no admin handle; the build fails if rx_capq.o
 * references an authority admin operation.
 */
#ifndef RX_CAPQ_H
#define RX_CAPQ_H

#include "rx_graph.h"

#include <stddef.h>
#include <stdint.h>

#define CQ_MAX_K        16u     /* candidates one query may return into cognition */
#define CQ_MAX_PLAN_OPS 32u     /* operation plus its specializations */

/* Sources a capability can come from. */
enum { CQ_SRC_GRAPH = 0,        /* Capability Graph (native) */
       CQ_SRC_SKILL,            /* Skill Network */
       CQ_SRC_MCP,              /* MCP registry */
       CQ_SRC_PHYSICAL,         /* local physical capability */
       CQ_SRC_FABRIC,           /* advertised by another machine */
       CQ_SOURCES };
#define CQ_SRC(s)       (1u << (s))
#define CQ_SRC_ALL      ((1u << CQ_SOURCES) - 1u)

/* Effect classes (aien-architecture docs/04). A provider declares every
 * class it may have; the need lists the classes it accepts. */
#define CQ_FX_PURE                  0x001u
#define CQ_FX_READ_FILESYSTEM       0x002u
#define CQ_FX_READ_NETWORK          0x004u
#define CQ_FX_SPAWN_PROCESS         0x008u
#define CQ_FX_LOCAL_EPHEMERAL       0x010u
#define CQ_FX_WORLD_MUTATION        0x020u
#define CQ_FX_EXTERNAL_WRITE        0x040u
#define CQ_FX_EXTERNAL_IRREVERSIBLE 0x080u
#define CQ_FX_SECRET_BEARING        0x100u

/* Locality. */
#define CQ_LOC_LOCAL    0x1u    /* this machine */
#define CQ_LOC_FABRIC   0x2u    /* another machine of the house Fabric */

/* Evidence levels, weakest first. */
enum { CQ_EV_NONE = 0,
       CQ_EV_DECLARED,          /* the provider says so */
       CQ_EV_MEASURED,          /* we measured it */
       CQ_EV_RECEIPT };         /* a bound qualification receipt */

#define CQ_PPM 1000000u

typedef struct {
    uint32_t semantic_operation;        /* operation id, or an alias the catalog resolves */
    uint64_t accepted_input_types;      /* types the caller can supply (bit per type) */
    uint64_t required_output_types;     /* types the result must include */
    uint32_t effect_class;              /* accepted effect classes (CQ_FX_*) */
    struct {
        uint64_t resource_lo, resource_hi;  /* the resource range it may touch */
        uint32_t rights;                    /* the most rights it may use */
    } authority_ceiling;
    struct {
        uint32_t allowed;               /* CQ_LOC_* */
        uint32_t machine;               /* 0 = any machine, else only this one */
    } locality_constraints;
    uint64_t latency_budget;            /* microseconds; 0 = no budget */
    uint64_t energy_budget;             /* microjoules; 0 = no budget */
    uint32_t reliability_requirement;   /* minimum, parts per million */
    uint32_t evidence_requirement;      /* minimum CQ_EV_* */
    uint32_t sources;                   /* optional restriction (CQ_SRC mask); 0 = all */
} CqNeed;

typedef struct {
    uint32_t capability_id;
    uint32_t skill_id;                  /* Skill Net procedure that realizes it (0 = none) */
    uint32_t machine_id;
    uint32_t realization_id;
    struct {
        uint64_t resource;
        uint32_t rights;
        uint32_t held;                  /* 1: the principal holds a valid capability now */
    } required_authority;
    uint64_t expected_cost;             /* abstract cost units */
    uint64_t expected_latency;          /* microseconds */
    uint64_t expected_energy;           /* microjoules */
    uint32_t confidence;                /* parts per million */
    uint32_t reliability;               /* parts per million */
    struct {
        uint64_t ref;                   /* receipt or measurement id */
        uint32_t level;                 /* CQ_EV_* */
    } evidence;
    uint8_t source;                     /* CQ_SRC_* */
    uint8_t local;                      /* 1: runs on this machine */
    uint16_t effects;                   /* CQ_FX_* the provider may have */
} CqCandidate;

/* Ranking dimensions. Each is turned into a value to minimize. */
enum { CQ_DIM_COST = 0, CQ_DIM_LATENCY, CQ_DIM_ENERGY, CQ_DIM_CONFIDENCE,
       CQ_DIM_RELIABILITY, CQ_DIM_AUTHORITY /* held first */, CQ_DIM_LOCALITY /* local first */,
       CQ_DIMS };

typedef struct {
    uint32_t n_order;
    uint8_t order[CQ_DIMS];             /* dimensions that matter, most important first */
    uint32_t tolerance_ppm[CQ_DIMS];    /* within best * (1 + tol) counts as tied (by dimension) */
    uint64_t max_cost;                  /* 0 = none */
    uint32_t min_confidence;            /* ppm */
    uint32_t k;                         /* at most this many (<= CQ_MAX_K) */
    uint32_t include_dominated;         /* 0: Pareto front only */
} CqTradeoffs;

/* ---- catalog (the Capability Graph and its sources) ---- */

typedef struct {
    uint32_t capability_id, skill_id, machine_id, realization_id;
    uint32_t op;                        /* canonical operation */
    uint8_t source;                     /* CQ_SRC_* */
    uint8_t evidence_level;
    uint16_t effects;
    uint64_t in_types;                  /* inputs it requires */
    uint64_t out_types;                 /* outputs it produces */
    uint64_t auth_resource;
    uint32_t auth_rights;
    uint32_t confidence_ppm, reliability_ppm;
    uint64_t cost, latency_us, energy_uj;
    uint64_t evidence_ref;
    uint32_t live;                      /* 0: withdrawn, or its MCP session is gone */
    uint32_t desc_len;                  /* description text, kept in the catalog */
    uint64_t desc_off;
} CqEntry;

typedef struct {
    uint32_t parent;                    /* 0 = none */
    uint32_t domains;                   /* CQ_SRC mask of sources that can realize it */
    uint32_t first_child, next_sibling;
} CqOp;

typedef struct {
    uint32_t self_machine;
    uint32_t n_ops, cap_ops;
    CqOp *ops;                          /* indexed by op id, 1..n_ops */
    uint32_t alias_mask;
    uint32_t *alias_key, *alias_op;     /* open addressing; key 0 = empty */
    uint32_t n_alias;
    uint32_t mach_mask;
    uint32_t *mach_key;
    uint64_t *mach_lease;               /* lease end, microseconds */
    uint32_t n_mach;
    uint32_t n, cap;
    CqEntry *e;
    char *desc;
    uint64_t desc_bytes, desc_cap;
    uint32_t *bucket_start;             /* (op, source) -> first index in bucket[] */
    uint32_t *bucket;
    int built;
} CqCatalog;

#define CQ_OK           0
#define CQ_E_ARG       -1
#define CQ_E_NOMEM     -2
#define CQ_E_NO_OP     -3               /* the need names no known operation */
#define CQ_E_NO_SOURCE -4               /* no source may realize it under the constraints */
#define CQ_E_UNBUILT   -5
#define CQ_E_PLAN_FULL -6               /* more specializations than CQ_MAX_PLAN_OPS */

int  cq_catalog_init(CqCatalog *c, uint32_t self_machine, uint32_t alias_capacity,
                     uint32_t machine_capacity);
void cq_catalog_free(CqCatalog *c);
/* Operations are numbered 1..n and defined parent first. */
int  cq_op_define(CqCatalog *c, uint32_t op, uint32_t parent, uint32_t domains);
int  cq_alias(CqCatalog *c, uint32_t alias, uint32_t op);
/* A machine's Fabric advertisement; its capabilities are live until the lease ends. */
int  cq_machine_advertise(CqCatalog *c, uint32_t machine, uint64_t lease_until_us);
int  cq_register(CqCatalog *c, const CqEntry *e, const char *desc, uint32_t desc_len);
/* Build the (operation, source) index. Registration order does not matter. */
int  cq_catalog_build(CqCatalog *c);
/* Alias or operation id -> canonical operation, 0 if unknown. */
uint32_t cq_resolve(const CqCatalog *c, uint32_t name);
/* Bytes the catalog holds (entries, index, descriptions). */
uint64_t cq_catalog_bytes(const CqCatalog *c);

/* ---- compile ---- */

typedef struct {
    int verdict;                        /* CQ_OK or CQ_E_* */
    uint32_t op;                        /* canonical operation */
    uint32_t n_ops;
    uint32_t ops[CQ_MAX_PLAN_OPS];      /* op and its specializations, sorted */
    uint32_t sources;                   /* sources probed */
    uint32_t n_probes;                  /* (operation, source) buckets */
    uint8_t digest[32];                 /* independent of how the operation was named */
} CqPlan;

/* Need -> plan. Resolves aliases, expands specializations, and keeps only
 * sources that can realize the operation and that the locality allows. */
int cq_compile(const CqCatalog *c, const CqNeed *need, CqPlan *plan);

/* ---- run ---- */

/* What crosses into cognition. */
typedef struct {
    int32_t verdict;
    uint32_t n;                         /* candidates returned */
    uint32_t n_feasible;                /* how many satisfied the need */
    uint32_t n_front;                   /* how many no other feasible one beat */
    CqCandidate cand[CQ_MAX_K];
} CqResult;

/* Bytes of r that reach cognition: the header and the returned candidates. */
size_t cq_result_bytes(const CqResult *r);

/* Optional: decide `held` through the world's authority view. */
typedef struct {
    const RxWorld *w;
    const AgCapTable *caps;             /* the principal's table */
} CqHeld;

typedef struct {
    uint64_t probed;                    /* entries looked at */
    uint64_t feasible;
    uint64_t rejected_dead;             /* withdrawn, dead session, expired lease */
    uint64_t desc_bytes_read;           /* always 0: the query never reads descriptions */
} CqStats;

int cq_query(const CqCatalog *c, const CqPlan *plan, const CqNeed *need,
             const CqTradeoffs *t, const CqHeld *held, uint64_t now_us,
             CqResult *out, CqStats *stats);

/* The same ranking on a caller's set (used by the query; exposed for tests).
 * Orders `in` into `out` (at most k) and returns how many, or CQ_E_NOMEM;
 * `front` gets the size of the Pareto front over the named dimensions. */
int cq_rank(const CqCandidate *in, uint32_t n, const CqTradeoffs *t,
            CqCandidate *out, uint32_t k, uint32_t *front);

/* Value to minimize on a dimension. */
uint64_t cq_dim_value(const CqCandidate *c, uint32_t dim);

/* ---- consumption by the action graph ---- */

/* Point an AG_SKILL node at the chosen candidate's procedure and declare the
 * authority it needs. Compilation then finds it held or reports it missing. */
int cq_bind_skill_node(AgGraph *g, uint32_t node, const CqCandidate *c);

const char *cq_source_name(uint32_t s);

#endif /* RX_CAPQ_H */
