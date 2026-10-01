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
 *
 * M20: this catalog IS the canonical Capability Graph. One entry says: this
 * capability (capability_id, a semantic operation), provided by this provider
 * (realization_id; a Skill when skill_id != 0, with skill_version and
 * skill_digest = AgSkill.identity of the procedure), on this machine, needs
 * this authority (a requirement, never a grant), under these constraints
 * (types, effects, cost, latency, energy), with this availability (`live`)
 * and this evidence (level + ref). An entry is keyed by CqKey; registering an
 * existing key updates it in place.
 *
 * Machines. CqEntry/CqCandidate/CqNeed carry a uint32 machine INDEX, valid in
 * this process only. A catalog made with cq_catalog_init_canonical resolves
 * every index through the caller's AienMachineIndex (aien_machine_id.h);
 * anything persisted or sent carries the 44-byte AienMachineId record instead
 * (see the wire form below). A catalog made with cq_catalog_init has no
 * identity table: it ranks and routes, but cannot export or ingest records.
 */
#ifndef RX_CAPQ_H
#define RX_CAPQ_H

#include "aien_machine_id.h"
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
    uint32_t require_held;              /* 1: only candidates whose authority is held now */
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
    uint32_t live;                      /* CQ_LIVE_*: withdrawn, available, unavailable */
    uint32_t desc_len;                  /* description text, kept in the catalog */
    uint64_t desc_off;
    /* M20 Capability Graph fields (zero = none / unpinned). */
    uint32_t skill_version;             /* version of skill_id's procedure */
    uint8_t skill_digest[32];           /* AgSkill.identity the provider claims */
    uint64_t generation;                /* advertiser's revision; newer replaces older */
} CqEntry;

/* Availability (CqEntry.live). Withdrawn is final for that registration: only
 * a new registration of the key brings it back. */
#define CQ_LIVE_WITHDRAWN   0u
#define CQ_LIVE_AVAILABLE   1u
#define CQ_LIVE_UNAVAILABLE 2u          /* registered, not usable now (busy, session down) */

/* An entry's identity: which capability, from which provider, on which
 * machine, through which Skill. Same tuple the ranking uses to break ties. */
typedef struct {
    uint32_t capability_id, realization_id, machine_id, skill_id;
} CqKey;

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
    AienMachineIndex *machines;         /* canonical identity behind each index (may be NULL) */
    uint32_t key_mask;                  /* CqKey -> entry: open addressing, slot = index + 1 */
    uint32_t *key_slot;
} CqCatalog;

#define CQ_OK           0
#define CQ_E_ARG       -1
#define CQ_E_NOMEM     -2
#define CQ_E_NO_OP     -3               /* the need names no known operation */
#define CQ_E_NO_SOURCE -4               /* no source may realize it under the constraints */
#define CQ_E_UNBUILT   -5
#define CQ_E_PLAN_FULL -6               /* more specializations than CQ_MAX_PLAN_OPS */
#define CQ_E_NOT_FOUND -7               /* no entry with that key */
#define CQ_E_WITHDRAWN -8               /* the entry was withdrawn; register it again */
#define CQ_E_NO_IDENTITY -9             /* no AienMachineIndex, or the index is unbound */
#define CQ_E_WIRE      -10              /* malformed, foreign or self-originated record */
#define CQ_E_STALE     -11              /* record older than (or as old as) what is held */

/* Index-only catalog: self_machine is a local runtime index. */
int  cq_catalog_init(CqCatalog *c, uint32_t self_machine, uint32_t alias_capacity,
                     uint32_t machine_capacity);
/* Canonical catalog: binds `self` in `machines` (caller-owned, must outlive
 * the catalog) and resolves every machine index through it. */
int  cq_catalog_init_canonical(CqCatalog *c, AienMachineIndex *machines, const AienMachineId *self,
                               uint32_t alias_capacity, uint32_t machine_capacity);
void cq_catalog_free(CqCatalog *c);
/* Canonical identity <-> local index (CQ_E_NO_IDENTITY without a table). */
int  cq_machine_identity(const CqCatalog *c, uint32_t machine, AienMachineId *out);
uint32_t cq_machine_index(const CqCatalog *c, const AienMachineId *m);   /* 0 if not bound */
/* Fabric advertisement by canonical identity: binds an index, sets the lease. */
int  cq_machine_advertise_id(CqCatalog *c, const AienMachineId *m, uint64_t lease_until_us,
                             uint32_t *index_out);
/* Operations are numbered 1..n and defined parent first. */
int  cq_op_define(CqCatalog *c, uint32_t op, uint32_t parent, uint32_t domains);
int  cq_alias(CqCatalog *c, uint32_t alias, uint32_t op);
/* A machine's Fabric advertisement; its capabilities are live until the lease ends. */
int  cq_machine_advertise(CqCatalog *c, uint32_t machine, uint64_t lease_until_us);
/* Register, or update in place when the entry's CqKey is already present
 * (a withdrawn entry is revived). An update that keeps op and source keeps
 * the index built; anything else needs cq_catalog_build again. An update
 * with no description keeps the old one. */
int  cq_register(CqCatalog *c, const CqEntry *e, const char *desc, uint32_t desc_len);
CqKey cq_key_of(const CqEntry *e);
CqKey cq_key_of_candidate(const CqCandidate *c);
/* The entry for a key, or NULL. The pointer is valid until the next register. */
const CqEntry *cq_lookup(const CqCatalog *c, const CqKey *k);
/* Availability: CQ_LIVE_AVAILABLE or CQ_LIVE_UNAVAILABLE. Never needs a rebuild. */
int  cq_set_availability(CqCatalog *c, const CqKey *k, uint32_t live);
/* Withdraw: the entry stays in the index but no query returns it again. */
int  cq_withdraw(CqCatalog *c, const CqKey *k);
/* Build the (operation, source) index. Registration order does not matter. */
int  cq_catalog_build(CqCatalog *c);
/* Digest of the operation catalog (tree, domains, aliases). Two nodes may
 * exchange wire records only when their digests match (Fabric handshake). */
void cq_ontology_digest(const CqCatalog *c, uint8_t out[32]);
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
    uint64_t rejected_unavailable;      /* registered but CQ_LIVE_UNAVAILABLE */
    uint64_t rejected_authority;        /* require_held and not held */
    uint64_t rejected_admission;        /* refused by the caller's admit function */
} CqStats;

int cq_query(const CqCatalog *c, const CqPlan *plan, const CqNeed *need,
             const CqTradeoffs *t, const CqHeld *held, uint64_t now_us,
             CqResult *out, CqStats *stats);

/* Admission before ranking: return nonzero to keep a feasible candidate. The
 * Pareto front and the order are computed over the admitted set only, so a
 * dominated but admissible provider is not lost behind an inadmissible one.
 * The function reads; it must not change the catalog or mint authority. */
typedef int (*CqAdmitFn)(void *ctx, const CqEntry *e, const CqCandidate *c);

int cq_query_admit(const CqCatalog *c, const CqPlan *plan, const CqNeed *need,
                   const CqTradeoffs *t, const CqHeld *held, uint64_t now_us,
                   CqAdmitFn admit, void *admit_ctx, CqResult *out, CqStats *stats);

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

/* ---- wire form (Fabric advertisement of Capability Graph entries) ----
 *
 * One fixed-size record per entry, CQ_WIRE_BYTES long. Every multi-byte
 * integer is little-endian; the machine is the 44-byte AienMachineId record,
 * never an index. No description text, no pointer, no local index, no lease.
 *
 *   off len field
 *     0   4 magic "ACGR"
 *     4   1 version 0x01
 *     5   1 kind           CQ_WIRE_ADVERTISE | CQ_WIRE_AVAILABILITY | CQ_WIRE_WITHDRAW
 *     6   1 source         the provider's source on its own machine (never FABRIC)
 *     7   1 evidence_level CQ_EV_*
 *     8  44 machine        AienMachineId record (aien_machine_id.h)
 *    52   4 capability_id
 *    56   4 realization_id (the provider)
 *    60   4 skill_id       0 = not a Skill
 *    64   4 skill_version
 *    68  32 skill_digest   AgSkill.identity (zero = unpinned)
 *   100   4 op             semantic operation id of the shared operation catalog
 *   104   2 effects        CQ_FX_*
 *   106   2 live           CQ_LIVE_*
 *   108   8 in_types
 *   116   8 out_types
 *   124   8 auth_resource  authority REQUIRED on the provider's machine (not a grant)
 *   132   4 auth_rights
 *   136   4 confidence_ppm
 *   140   4 reliability_ppm
 *   144   8 cost
 *   152   8 latency_us
 *   160   8 energy_uj
 *   168   8 evidence_ref
 *   176   8 generation     advertiser's revision of this entry; must increase
 *   184   4 check          first 4 bytes of SHA-256(bytes 0..183)
 *
 * Rules. Only a machine's own entries are exported (machine = self, source !=
 * FABRIC): nobody re-advertises another machine's capability. Ingestion
 * refuses records naming the receiver itself, records whose op the receiver's
 * operation catalog does not know (the handshake compares cq_ontology_digest),
 * and records whose generation is not newer than the one held. An ingested
 * entry becomes source CQ_SRC_FABRIC on its machine's index; the lease is the
 * receiver's (cq_machine_advertise_id from the Fabric session), not the
 * record's. Authenticating the sender is Fabric's job; this code checks form.
 */
#define CQ_WIRE_BYTES        188u
#define CQ_WIRE_VERSION      0x01u
#define CQ_WIRE_ADVERTISE    1u     /* register or update */
#define CQ_WIRE_AVAILABILITY 2u     /* `live` changed */
#define CQ_WIRE_WITHDRAW     3u

/* Export one of this machine's entries. */
int cq_wire_encode(const CqCatalog *c, const CqEntry *e, uint32_t kind, uint8_t out[CQ_WIRE_BYTES]);
/* Parse and check a record: no catalog change. out->machine_id is 0; the
 * machine is returned in *machine. */
int cq_wire_decode(const uint8_t *in, size_t len, uint32_t *kind, AienMachineId *machine,
                   CqEntry *out);
/* Ingest a record from another machine into a canonical catalog. On success
 * *key_out (optional) is the entry's local key. */
int cq_wire_apply(CqCatalog *c, const uint8_t *in, size_t len, CqKey *key_out);

#endif /* RX_CAPQ_H */
