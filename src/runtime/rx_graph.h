/*
 * rx_graph.h -- Omega action graph IR (OMEGA_ACTION_GRAPH_IR).
 *
 * Routine multi-step behaviour does not go back through deliberation every
 * time. AIEN states a goal; Omega compiles it, with the World state, the
 * capabilities its principal already holds and the goal's constraints, into a
 * typed dependency graph. Omega checks the graph, optimizes it, and lowers
 * each node onto one resident reaction (rx_world.h). From then on the graph
 * runs by readiness alone: a node runs when the cells it depends on hold this
 * run's values. No function sequences the nodes. Independent nodes run on
 * whatever workers are free.
 *
 *   Goal + World + Capabilities + Constraints
 *        -> rx_graph_compile   instantiate a procedure, bind World objects,
 *                              resolve authority (never mint), check resources,
 *                              generations and constraints, type check
 *        -> rx_graph_optimize  semantic-preserving passes
 *        -> rx_graph_lower     one cell object + one reaction per node
 *        -> rx_graph_start     publish a run token; the world does the rest
 *
 * The graph is typed Omega state, not text: nodes, typed data edges, control
 * dependencies, authority and resource requirements, success and failure
 * conditions, evidence requirements and effect boundaries. Its identity is
 * SHA-256 over a canonical encoding that does not depend on builder order.
 *
 * Authority. Every node declares what it needs. Compilation looks it up in
 * the principal's capability table and validates it through the world's
 * authority view. It has no path to mint: this file takes no admin handle.
 * A requirement that is missing is reported. When an AEGIS client binding is
 * given to the lowering, the node reads its authority from a capability slot
 * and an ask reaction publishes the request, so new authority arrives only
 * through AEGIS policy and the AIENOS root (rx_aegis.h). The engine checks
 * every node's authority again when it starts and when it publishes.
 *
 * Effects. Proposing an effect (EFFECT_PROPOSE) is thinking: a value, no
 * authority, no change outside the graph. Performing it (EFFECT_PERFORM) is
 * an explicit node that needs WRITE|EFFECT on the effect's resource, a
 * passing verdict, and its place in the graph's effect chain. World
 * publications are effect boundaries too. Passes never merge, remove or
 * reorder boundary nodes.
 */
#ifndef RX_GRAPH_H
#define RX_GRAPH_H

#include "rx_world.h"
#include "rx_aegis.h"

#include <stddef.h>
#include <stdint.h>

#define AG_MAX_NODES     64u
#define AG_MAX_IN        4u     /* data inputs per node */
#define AG_MAX_EDGES     (AG_MAX_NODES * AG_MAX_IN)
#define AG_MAX_DEPS      (AG_MAX_NODES * 2u)
#define AG_MAX_FUSED     8u
#define AG_MAX_REQS      AG_MAX_NODES
#define AG_MAX_CONDS     8u
#define AG_MAX_SKILLS    16u
#define AG_MAX_CAPS      16u
#define AG_MAX_PROCS     8u
#define AG_META_MAX_STEPS 8u    /* steps one fused node (MetaSkill) runs */
#define AG_META_MAX_OBJ   2u    /* World objects a fused node reads */
#define AG_META_TABLE     64u   /* entries of a learned realization */

/* Value types carried on data edges. */
typedef enum {
    AG_T_NONE = 0,
    AG_T_U64,
    AG_T_BOOL,
    AG_T_PROPOSAL,      /* a proposed effect: thought, not performed */
    AG_T_VERDICT,       /* a verification outcome */
    AG_T_RECEIPT        /* what a performed effect or publication returns */
} AgType;

typedef enum {
    AG_CONST = 1,
    AG_PURE,            /* Omega pure operation (omega_eval_pure_binary_uint), maybe fused */
    AG_WORLD_READ,      /* read one field of a World object */
    AG_WORLD_PUBLISH,   /* write one field of a World object (effect boundary) */
    AG_RECALL,          /* Cortex memory lookup by key */
    AG_CAP_RESOLVE,     /* does the principal hold this authority? decided at compile */
    AG_PHYSICAL,        /* physical computation with a declared resource need */
    AG_SKILL,           /* reusable procedure from the Skill Net table */
    AG_EFFECT_PROPOSE,  /* thinking about an effect */
    AG_EFFECT_PERFORM,  /* performing it (effect boundary) */
    AG_VERIFY,          /* verdict: input within [imm, imm2] */
    AG_BRANCH,          /* bool decision other nodes are guarded by */
    AG_JOIN,            /* the one input that is OK */
    AG_RETRY,           /* bounded retry of a skill */
    AG_META,            /* a published MetaSkill: several verified steps in one node (rx_fusion.h) */
    AG_KIND_COUNT
} AgKind;

/* Data edge modes. ON_FAIL runs its node only when the source failed. */
enum { AG_EDGE_DATA = 0, AG_EDGE_ON_FAIL = 1 };

/* Control dependency kinds. */
enum { AG_DEP_GUARD = 1,    /* run only when the source's bool equals polarity */
       AG_DEP_ORDER = 2 };  /* wait for the source to be resolved (effect chain) */

/* Node status in a run. */
enum { AG_PENDING = 0, AG_OK = 1, AG_FAILED = 2, AG_SKIPPED = 3 };

/* Graph outcome. */
enum { AG_RUN_SUCCESS = 1, AG_RUN_FAILURE, AG_RUN_INCOMPLETE };

/* Compile verdicts and errors. */
#define AG_OK_READY            0
#define AG_E_ARG              -1
#define AG_E_FULL             -2
#define AG_E_TYPE             -3
#define AG_E_CYCLE            -4
#define AG_E_STALE            -5
#define AG_E_CONSTRAINT       -6
#define AG_E_NO_PROCEDURE     -7
#define AG_E_EFFECT_ORDER     -8
#define AG_E_DEP_BUDGET       -9
#define AG_E_JOIN             -10
#define AG_E_LOWER            -11

typedef struct {
    uint32_t op;                /* OpCode */
    uint64_t imm;               /* right operand */
} AgFusedStep;

typedef struct {
    AgKind kind;
    AgType out_type;
    uint32_t op;                /* PURE: OpCode; SKILL/RETRY: skill id */
    uint64_t imm, imm2;         /* CONST value; PURE right operand when unary; RECALL key;
                                   VERIFY bounds; CAP_RESOLVE resource/rights; RETRY
                                   max attempts / leading failures of the stand-in skill */
    uint32_t param;             /* procedure parameter the World object binds from (1-based) */
    RxObjRef obj;               /* READ/PUBLISH/RECALL/PERFORM target */
    uint32_t field;
    uint32_t cost_us;           /* work this node really does (busy time) */
    uint32_t n_fused;           /* PURE after fusion: steps applied after the head op */
    AgFusedStep fused[AG_MAX_FUSED];
    uint8_t id[32];             /* semantic identity (rx_graph_identify) */
    uint32_t origin;            /* index in the graph it was first built in */
    uint8_t alive;
    /* META only. op = skill id; imm, imm2 = the first 128 bits of the
     * realization identity the node was built for. */
    uint32_t n_mobj;
    RxObjRef mobj[AG_META_MAX_OBJ];     /* World objects its steps read, by slot */
    AgType in_type[AG_MAX_IN];          /* input contract, by port */
} AgNode;

typedef struct { uint16_t from, to; uint8_t port, mode; AgType type; } AgDataEdge;
typedef struct { uint16_t from, to; uint8_t kind, polarity; } AgDep;
typedef struct { uint16_t node; uint64_t resource; uint32_t rights; } AgAuthReq;
typedef struct { uint16_t node; RxResourceNeed need; } AgResReq;
typedef struct { uint16_t node; uint8_t status; } AgCond;

typedef struct {
    uint32_t n_nodes;
    AgNode nodes[AG_MAX_NODES];
    uint32_t n_data;
    AgDataEdge data[AG_MAX_EDGES];
    uint32_t n_deps;
    AgDep deps[AG_MAX_DEPS];
    uint32_t n_auth;
    AgAuthReq auth[AG_MAX_REQS];
    uint32_t n_res;
    AgResReq res[AG_MAX_REQS];
    uint32_t n_success;
    AgCond success[AG_MAX_CONDS];       /* all must hold for AG_RUN_SUCCESS */
    uint32_t n_failure;
    AgCond failure[AG_MAX_CONDS];       /* any one makes AG_RUN_FAILURE */
    uint32_t n_evidence;
    uint16_t evidence[AG_MAX_NODES];    /* nodes whose evidence must exist after a run */
    uint32_t n_effects;
    uint16_t effects[AG_MAX_NODES];     /* effect boundaries, in their required order */
    uint32_t subject;                   /* the principal the graph runs as */
    uint8_t digest[32];
} AgGraph;

/* ---- building ---- */

void rx_graph_init(AgGraph *g, uint32_t subject);
int  rx_graph_node(AgGraph *g, AgKind kind, AgType out);     /* index or AG_E_FULL */
int  rx_graph_data(AgGraph *g, uint32_t from, uint32_t to, uint32_t port, uint32_t mode);
int  rx_graph_guard(AgGraph *g, uint32_t branch, uint32_t node, uint32_t polarity);
int  rx_graph_order(AgGraph *g, uint32_t before, uint32_t after);
int  rx_graph_need_authority(AgGraph *g, uint32_t node, uint64_t resource, uint32_t rights);
int  rx_graph_need_resource(AgGraph *g, uint32_t node, const RxResourceNeed *need);
int  rx_graph_success(AgGraph *g, uint32_t node, uint32_t status);
int  rx_graph_failure(AgGraph *g, uint32_t node, uint32_t status);
int  rx_graph_evidence(AgGraph *g, uint32_t node);

/* Declares every derivable requirement (authority on READ/PUBLISH/RECALL/
 * PERFORM targets, evidence on VERIFY/PERFORM/PUBLISH, effect boundaries in
 * chain order), then checks: types, acyclicity, one total order over effect
 * boundaries, joins fed by mutually exclusive arms, the per-node dependency
 * budget. Fills identities and the digest. */
int  rx_graph_validate(AgGraph *g, const RxWorld *w);

/* Semantic identity of every node and the graph digest. */
void rx_graph_identify(AgGraph *g);

/* ---- compile ---- */

/* A reusable procedure (Skill Net). Deterministic function of its inputs. */
typedef uint64_t (*AgSkillFn)(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed);

/* ---- MetaSkill realizations (what an AG_META node runs) ----
 *
 * A step program is a fused fragment: its steps in a canonical order (the
 * exit last), each step an ordinary node whose inputs are either an external
 * port of the fused node or an earlier step. Every step runs through the same
 * node semantics as the graph it came from. */
typedef struct {
    AgNode node;                /* kind, out type, op, imm, imm2, field, cost, fused steps */
    uint32_t n_in;
    struct { uint8_t step; uint8_t index; uint8_t mode; } in[AG_MAX_IN];
                                /* step 1: earlier step `index`; 0: external port `index` */
    int32_t obj;                /* object slot it reads, or -1 */
    uint8_t local_id[32];       /* identity inside the fragment (not the graph around it) */
} AgMetaStep;

typedef struct {
    uint32_t n_steps;
    AgMetaStep step[AG_META_MAX_STEPS];
    uint32_t n_in;
    AgType in_type[AG_MAX_IN];
    uint8_t in_mode[AG_MAX_IN];         /* AG_EDGE_DATA or AG_EDGE_ON_FAIL, per port */
    uint32_t n_obj;
    uint32_t obj_rights[AG_META_MAX_OBJ];
    RxResourceNeed need;                /* merged resource need of the steps */
    AgType out_type;
    uint8_t signature[32];              /* steps, internal dependencies, authority pattern, contracts */
} AgMetaProgram;

typedef enum {
    AG_REAL_COMPILED = 1,       /* deterministic: the step program, register-resolved, one reaction */
    AG_REAL_LEARNED,            /* a table learned from verified observations; nothing else */
    AG_REAL_HYBRID,             /* the learned table, and the step program on a miss */
    AG_REAL_HARDWARE            /* a hardware realization; none exists on this host */
} AgRealKind;

/* A learned entry: inputs (status and value by port) and the World fields
 * read, to the result and the step evidence the reference produced. */
typedef struct {
    uint8_t s[AG_MAX_IN];
    uint64_t v[AG_MAX_IN];
    uint64_t world;             /* digest of the fields of every object slot */
    uint8_t status;
    uint64_t value;
    uint64_t steps;
    uint32_t attempts;
} AgMetaEntry;

typedef struct {
    AgRealKind kind;
    AgMetaProgram prog;         /* COMPILED, HYBRID: what runs; others: the contract it serves */
    uint32_t n_table;
    AgMetaEntry table[AG_META_TABLE];
    uint8_t identity[32];       /* rx_graph_realization_identity */
} AgRealization;

typedef struct {
    uint32_t id;
    AgSkillFn fn;
    uint8_t identity[32];       /* content identity of the procedure */
} AgSkill;

typedef struct {
    uint32_t n;
    AgSkill skill[AG_MAX_SKILLS];
    /* Published MetaSkills: what an AG_META node with this skill id runs.
     * Only rx_fusion_publish adds here, after the generation barrier. */
    uint32_t n_meta;
    struct { uint32_t id; const AgRealization *r; } meta[AG_MAX_SKILLS];
} AgSkillTable;

/* A procedure: a graph template whose World objects are parameters. */
typedef struct {
    uint32_t goal_kind;
    const AgGraph *tmpl;
} AgProcedure;

typedef struct {
    uint32_t n;
    AgProcedure proc[AG_MAX_PROCS];
} AgLibrary;

typedef struct {
    uint32_t kind;              /* selects the procedure */
    uint32_t n_args;
    RxObjRef args[8];           /* World objects bound to parameters 1..n */
} AgGoal;

/* Capabilities the principal already holds. Compile only reads this. */
typedef struct {
    uint32_t subject;           /* the principal these belong to */
    uint32_t n;
    struct { RxCapRef ref; uint64_t resource; uint32_t rights; } cap[AG_MAX_CAPS];
} AgCapTable;

typedef struct {
    uint32_t max_effects;       /* 0 = none allowed; UINT32_MAX = any */
    uint64_t forbid_lo, forbid_hi;  /* no node may touch these resources (hi 0 = none) */
    uint64_t critical_path_us;  /* 0 = no deadline */
    RxResourceBudget budget;    /* what the body offers now */
} AgConstraints;

typedef struct {
    int verdict;                /* AG_OK_READY or AG_E_* */
    uint32_t n_missing;
    AgAuthReq missing[AG_MAX_REQS];     /* authority the principal does not hold */
    uint32_t n_resource_blocked;
    uint16_t resource_blocked[AG_MAX_NODES];
    uint32_t n_stale;
    uint16_t stale[AG_MAX_NODES];
    uint64_t critical_path_us;
    uint64_t total_work_us;
    uint32_t nodes_before, nodes_after;
    uint32_t reactions_before, reactions_after;
    struct { uint32_t const_folded, cse, world_reads, fused, dead, deps_dropped; } passes;
} AgReport;

/* Goal -> typed graph. Validates capabilities through w's authority view; it
 * never mints and has no handle that could. Runs rx_graph_optimize when
 * `optimize` is set. The graph is produced even when authority or resources
 * are missing (the report says what); it is refused for stale World
 * references, violated constraints, and type or structure errors. */
int rx_graph_compile(const AgGoal *goal, RxWorld *w, const AgCapTable *caps,
                     const AgConstraints *cons, const AgLibrary *lib, int optimize,
                     AgGraph *out, AgReport *rep);

/* The tail of compile: costs and the critical-path constraint, missing
 * authority, resource-blocked nodes. Also used after a graph is rewritten
 * (rx_fusion_apply). Returns AG_OK_READY or AG_E_CONSTRAINT. */
int rx_graph_report_needs(const AgGraph *g, RxWorld *w, const AgCapTable *caps,
                          const AgConstraints *cons, AgReport *rep);

/* Semantic-preserving passes, in order: constant propagation, common
 * subexpression elimination (identity-equal pure nodes and World reads with
 * no intervening publication of that object), pure-node fusion, dependency
 * simplification (implied order edges, order edges that only delay an
 * effect-free node), dead-node elimination (evidence, success/failure and
 * boundary nodes are roots). Parallelism needs no pass: lowering adds no order the
 * graph does not state. Returns 0 and fills rep->passes. */
int rx_graph_optimize(AgGraph *g, AgReport *rep);

/* 0 when `after` keeps every effect boundary of `before` (same identity,
 * same pairwise order), every evidence node, and every success/failure
 * condition; negative otherwise. */
int rx_graph_check_preserved(const AgGraph *before, const AgGraph *after);

/* Critical path and total work of the graph from node cost_us. */
void rx_graph_costs(const AgGraph *g, uint64_t *critical_us, uint64_t *total_us);

/* ---- lowering ---- */

/* Cell object type and layout: 0 run, 1 status, 2 value, 3 evidence,
 * 4 attempts. Every field is proposed on every run. */
#define AG_OT_CELL   0x5A01u
#define AG_OT_RUN    0x5A02u
#define AG_OT_EFFECT 0x5A03u   /* 0 count, 1 last value, 2 order chain, 3 last run */
#define AG_OT_MEMORY 0x5A04u   /* key/value pairs in fields (0,1) (2,3) (4,5) (6,7) */

/* Optional: route missing authority through an AEGIS client. */
typedef struct {
    const RxAegisClientObjects *client;
    RxCapRef request_cap;                   /* principal RW on its request object */
    RxCapRef slot_cap[RX_AEGIS_SLOTS];      /* principal R on each slot */
    uint64_t request_res, slot_res[RX_AEGIS_SLOTS];
} AgAegisBinding;

typedef struct AgLowered {
    RxWorld *w;
    const AgGraph *g;
    const AgSkillTable *skills;
    RxObjRef run;                           /* run token: field 0 = run id */
    RxObjRef cell[AG_MAX_NODES];
    uint32_t reaction[AG_MAX_NODES];
    uint32_t n_ask;
    uint32_t ask_reaction[RX_AEGIS_SLOTS];
    uint64_t cell_res, run_res;
    const RxAegisClientObjects *aegis_client;
    struct { uint32_t node; uint32_t slot; } asked[RX_AEGIS_SLOTS];
    AgAuthReq ask_req[RX_AEGIS_SLOTS];
    struct AgNodeCtx { const struct AgLowered *L; uint32_t node; } nctx[AG_MAX_NODES];
    struct AgAskCtx { const struct AgLowered *L; uint32_t k; } actx[RX_AEGIS_SLOTS];
} AgLowered;

/* Create the run token and one cell per live node (resource cell_res), and
 * one reaction per node. cell_cap: principal RW on cell_res; run_cap:
 * principal R on run_res. Missing authority lowers to an unusable reference
 * (the engine blocks the node and records it) or, with `aegis`, to a slot. */
int rx_graph_lower(AgLowered *L, RxWorld *w, const AgGraph *g, const AgSkillTable *skills,
                   const AgCapTable *caps, uint64_t cell_res, RxCapRef cell_cap,
                   uint64_t run_res, RxCapRef run_cap, const AgAegisBinding *aegis);

/* Publish run id `run` on the token. The only thing the caller does. */
int64_t rx_graph_start(AgLowered *L, RxCapRef external_run_cap, uint64_t run);

/* Read the run's per-node status/value and decide the outcome. */
typedef struct {
    uint64_t run;
    uint8_t status[AG_MAX_NODES];
    uint64_t value[AG_MAX_NODES];
    uint64_t evidence[AG_MAX_NODES];
    uint32_t attempts[AG_MAX_NODES];
    uint64_t steps[AG_MAX_NODES];       /* META: step evidence (rx_graph_step_record chain) */
    int outcome;
} AgResult;

int rx_graph_collect(AgLowered *L, uint64_t run, AgResult *out);

/* ---- reference ---- */

/* The same node semantics, one node at a time in a fixed topological order,
 * against a copy of the World. Effects and publications go to a shadow; the
 * World is not changed. `effect_chain` receives the chain value each effect
 * object would hold (indexed like g->effects). `stale` marks a node whose
 * World object is no longer live; it and its dependants stay PENDING. */
typedef struct {
    AgResult r;
    uint64_t effect_count;
    uint64_t effect_chain[AG_MAX_NODES];
    uint64_t published[AG_MAX_NODES];
    /* The World fields each node saw when it ran, by object slot (slot 0
     * for READ/RECALL/PERFORM; META by its own slots). */
    uint64_t world_seen[AG_MAX_NODES][AG_META_MAX_OBJ][RX_MAX_FIELDS];
} AgReference;

int rx_graph_reference(const AgGraph *g, RxWorld *w, const AgSkillTable *skills,
                       const AgCapTable *caps, uint64_t run, AgReference *out);

/* The reference against given object contents instead of a World: every
 * object named here is live with these fields, every other is stale, and
 * authority is not consulted. Used to test a fragment on inputs no World
 * holds. */
typedef struct {
    RxObjRef obj;
    uint64_t field[RX_MAX_FIELDS];
} AgObjContents;

int rx_graph_reference_on(const AgGraph *g, const AgSkillTable *skills, const AgObjContents *objs,
                          uint32_t n_objs, uint64_t run, AgReference *out);

/* ---- MetaSkill support ---- */

/* Deterministic topological order of the live nodes (index order breaks
 * ties). Count, or AG_E_CYCLE. */
int rx_graph_topo(const AgGraph *g, uint16_t order[AG_MAX_NODES]);

/* One link of a fused node's step evidence: the step's identity inside its
 * fragment, its status and value, and the input values it saw. */
uint64_t rx_graph_step_record(uint64_t acc, const uint8_t local_id[32], uint32_t status,
                              uint64_t value, const uint64_t *in, uint32_t n_in);

/* Run a realization on inputs (status and value by port) and the fields of
 * each object slot. Fills status, value, attempts, the step evidence and the
 * busy time the steps that ran would have taken. Returns 0, or -1 when the
 * realization cannot run here (HARDWARE, a LEARNED miss). */
typedef struct {
    uint8_t status;
    uint64_t value;
    uint32_t attempts;
    uint64_t steps;
    uint64_t busy_us;
    int table_hit;
} AgMetaOut;

int rx_graph_meta_run(const AgRealization *r, const AgSkillTable *skills, const uint8_t *s,
                      const uint64_t *v, uint32_t n_in,
                      const uint64_t world[AG_META_MAX_OBJ][RX_MAX_FIELDS], AgMetaOut *out);

/* Canonical bytes of a realization: its program as it will run and its
 * learned table. Returns the length (the bytes written are at most cap). */
#define AG_REALIZATION_MAX_BYTES 16384u
size_t rx_graph_realization_encode(const AgRealization *r, uint8_t *buf, size_t cap);

/* Content identity: SHA-256 over the canonical bytes. */
void rx_graph_realization_identity(AgRealization *r);

/* Fill every step's local identity and the program signature from the
 * steps themselves. */
void rx_graph_meta_seal(AgMetaProgram *p);

/* The learned-table key of the World fields a program reads. */
uint64_t rx_graph_meta_world_key(const AgMetaProgram *p,
                                 const uint64_t world[AG_META_MAX_OBJ][RX_MAX_FIELDS]);

/* Replace the live nodes in `members` (one of them `exit`, the only one whose
 * result leaves the set) with one AG_META node. ext[i] is the node feeding
 * port i; objs[j] the World object of slot j. Authority and resource
 * requirements of the members move to the new node, as do the exit's
 * conditions and the members' evidence obligations. The graph is compacted;
 * the caller validates it. Returns the new node's index or AG_E_*. */
int rx_graph_fuse(AgGraph *g, uint64_t members, uint32_t exit, uint32_t skill_id,
                  const AgRealization *r, const uint16_t *ext, uint32_t n_ext,
                  const RxObjRef *objs, uint32_t n_objs);

/* Order chain step used by effect objects (and the reference). */
uint64_t rx_graph_chain(uint64_t chain, uint64_t run, uint64_t value);

/* Per-node evidence word: identity, run, status, value, inputs. */
uint64_t rx_graph_evidence_word(const AgNode *n, uint64_t run, uint32_t status, uint64_t value,
                                const uint64_t *in, uint32_t n_in);

/* Static ancestors of `node` over data, guard and order edges (bit set). */
uint64_t rx_graph_ancestors(const AgGraph *g, uint32_t node);

const char *rx_graph_kind_name(AgKind k);

#endif /* RX_GRAPH_H */
