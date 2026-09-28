/*
 * rx_contract.h -- Omega typed result constraints (OMEGA_TYPED_RESULT_CONSTRAINTS).
 *
 * When cognition is asked for a structured semantic object (a plan, a
 * hypothesis, a capability need, an action graph, a skill invocation, a World
 * mutation, an effect proposal, a generation candidate, an evidence record),
 * the answer has an output contract. The contract is typed Omega state: typed
 * fields and a table of rules, with a SHA-256 identity over a canonical
 * binary encoding. JSON is one serialization of an object (rc_to_json /
 * rc_from_json). It is not the contract, and a JSON text is only accepted
 * after it has been parsed back into typed state and checked.
 *
 * Rule kinds: ranges, enums, structural invariants, cross-field relations,
 * authority, generation, referential integrity, required evidence and effect
 * restrictions. Field types are checked before any rule.
 *
 * Two paths to a published object:
 *
 *   constrained  the contract is compiled (rc_compile) into one allowed-value
 *                domain per field, in the contract's generation order, given
 *                the fields already chosen and the context. A backend that can
 *                decode under a mask (RcBackend.constrained) only ever chooses
 *                inside it. Rules that cannot become a per-field domain are
 *                checked once the candidate is complete. A domain that is
 *                empty ends the request before more is generated.
 *
 *   free         generate a candidate, check it, repair what the contract
 *                marks repairable, otherwise reject and generate again, up
 *                to a bound.
 *
 * Either way nothing reaches the World unchecked. The publish gate
 * (rc_gate_register) is a resident reaction: it is the only holder of WRITE
 * on the published object, it checks every draft again against the contract
 * and the live World, repairs only derived fields and narrowing, and publishes
 * the object or records the rejection. A producer that writes the published
 * object itself is blocked by the authority.
 *
 * Repair never invents meaning. Only two repairs exist: recompute a field the
 * contract marks as derived (a sequence number, a digest or count taken from
 * an attached graph, an object's resource), and clear bits a rule says may
 * not be set (asking for fewer rights). Stale generations, missing evidence,
 * wrong principals, broken references and effect restrictions are rejected.
 */
#ifndef RX_CONTRACT_H
#define RX_CONTRACT_H

#include "rx_graph.h"
#include "rx_world.h"

#include <pthread.h>
#include <stdint.h>

#define RC_MAX_FIELDS   8u
#define RC_MAX_RULES    24u
#define RC_MAX_SET      256u
#define RC_MAX_EVIDENCE 256u
#define RC_MAX_REFS     4u
#define RC_MAX_EFFECT_RES 8u
#define RC_NONE         0xffu   /* no field */

typedef enum {
    RC_PLAN = 1,
    RC_HYPOTHESIS,
    RC_CAPABILITY_NEED,
    RC_ACTION_GRAPH,
    RC_SKILL_INVOCATION,
    RC_WORLD_MUTATION,
    RC_EFFECT_PROPOSAL,
    RC_GENERATION_CANDIDATE,
    RC_EVIDENCE,
    RC_KIND_COUNT
} RcKind;

/* Field types. */
typedef enum {
    RC_T_U64 = 1,
    RC_T_U32,           /* <= UINT32_MAX */
    RC_T_BOOL,          /* 0 or 1 */
    RC_T_ENUM,          /* < 64 */
    RC_T_BITS,          /* a rights word, <= UINT32_MAX */
    RC_T_OBJ,           /* World object id, < RX_MAX_OBJECTS */
    RC_T_SUBJECT,       /* principal, 1..UINT32_MAX */
    RC_T_RESOURCE,      /* capability resource, nonzero */
    RC_T_DIGEST         /* one 64-bit word of a SHA-256 */
} RcType;

/* Constraint types (RcRule.kind). */
typedef enum {
    RC_RULE_RANGE = 1,      /* f[a] in [k, k2] */
    RC_RULE_ENUM,           /* f[a] in the value set k (bit v = value v, v < 64) */
    RC_RULE_STRUCT,         /* structural invariant, op RC_S_* */
    RC_RULE_CROSS,          /* when guard holds: f[a] cmp (b == RC_NONE ? k : f[b] + k) */
    RC_RULE_AUTHORITY,      /* op RC_A_* */
    RC_RULE_GENERATION,     /* op RC_G_* */
    RC_RULE_REFERENCE,      /* op RC_R_* */
    RC_RULE_EVIDENCE,       /* a PASS evidence record of subject kind k names f[a] */
    RC_RULE_EFFECT,         /* op RC_E_* */
    RC_RULE_KIND_COUNT
} RcRuleKind;

/* Rule ops. */
enum { RC_S_NONZERO = 1,        /* f[a] != 0 */
       RC_S_BITS_WITHIN,        /* f[a] & ~k == 0   (repair: narrow) */
       RC_S_ACTION_GRAPH,       /* attached graph validates, belongs to f[2], evidence per effect */
       RC_S_GRAPH_FIELDS };     /* fields 0,1,3,4,6 are the graph's digest and counts (derived) */
enum { RC_A_PRINCIPAL = 1,      /* f[a] == the producing principal */
       RC_A_HOLDS,              /* principal holds rights (b field, or k when b = NONE) on resource f[a] */
       RC_A_NO_PRIVILEGED };    /* f[a] & k == 0    (repair: narrow) */
enum { RC_G_LIVE = 1,           /* object f[a] is live at generation f[b] */
       RC_G_ACTIVE,             /* f[a] == the active generation */
       RC_G_NEXT,               /* f[a] > the active generation */
       RC_G_EPOCH };            /* f[a] == the authority epoch */
enum { RC_R_CTX_FIELD = 1,      /* f[a] == field k2 of context object k (b = 1: 0 also allowed) */
       RC_R_NEXT_SEQ,           /* f[a] == the context's next sequence number (derived) */
       RC_R_SKILL,              /* f[a] is a skill of the table */
       RC_R_ARITY,              /* f[a] == arity of skill f[b] */
       RC_R_OBJECT,             /* f[a] names a live World object */
       RC_R_RESOURCE_OF,        /* f[a] == the resource of live object f[b] (derived) */
       RC_R_RESOURCE_LIVE,      /* some live World object is guarded by resource f[a] */
       RC_R_CTX_VALUE };        /* f[a] == context value k (index into RcContext.values) */
enum { RC_E_FORBIDDEN = 1,      /* resource f[a] outside the forbidden range */
       RC_E_ALLOWED,            /* resource f[a] is an effect resource of the context, not forbidden */
       RC_E_BUDGET,             /* the context still has an effect left */
       RC_E_MAX,                /* f[a] <= effects the context has left */
       RC_E_APPROVAL,           /* when the guard holds, f[a] (an approval) != 0 */
       RC_E_GRAPH_CLEAR };      /* no requirement of the attached graph is forbidden */

/* Comparisons. */
enum { RC_EQ = 1, RC_NE, RC_LT, RC_LE, RC_GT, RC_GE };

/* Guard modes of a CROSS rule (and an EFFECT approval rule). */
enum { RC_ALWAYS = 0, RC_GUARD_IN = 1 /* f[g] < 64 and bit f[g] of kg */,
       RC_GUARD_BITS = 2 /* f[g] & kg != 0 */ };

/* Repair policy of a rule. */
enum { RC_FIX_NONE = 0,         /* violation rejects */
       RC_FIX_DERIVE,           /* recompute field a from the context / attached graph */
       RC_FIX_NARROW };         /* clear the offending bits of field a */

typedef struct {
    uint8_t kind, op;
    uint8_t a, b;               /* fields (RC_NONE = unused / constant) */
    uint8_t g, gmode;           /* guard field and mode */
    uint8_t cmp;                /* RC_EQ.. for CROSS */
    uint8_t repair;
    uint64_t k, k2, kg;
} RcRule;

typedef struct {
    const char *name;           /* serialization key; not part of the identity */
    RcType type;
    uint8_t derived;            /* recomputed, never meaning-bearing */
} RcFieldSpec;

typedef struct {
    RcKind kind;
    const char *name;
    uint32_t n_fields;
    RcFieldSpec field[RC_MAX_FIELDS];
    uint8_t order[RC_MAX_FIELDS];       /* generation order */
    uint32_t n_rules;
    RcRule rule[RC_MAX_RULES];
    uint32_t world_type;                /* object type of the published object */
    uint8_t identity[32];               /* SHA-256 over the canonical contract */
} RcContract;

/* A semantic object: typed state. The graph is attached for ACTION_GRAPH. */
typedef struct {
    RcKind kind;
    uint64_t f[RC_MAX_FIELDS];
    const AgGraph *graph;
} RcObject;

/* CAPABILITY_NEED: effect classes a need may accept, and where it may run. */
#define RC_EFFECT_CLASSES 0x3fu     /* pure, read, write, external, irreversible, physical */
#define RC_LOCALITIES     0x7u      /* this core class, this machine, the Fabric */

/* Evidence subject kinds (EVIDENCE field 0). */
enum { RC_EV_VERIFY = 1, RC_EV_GENERATION = 2, RC_EV_MEASURE = 3 };
enum { RC_VERDICT_PASS = 1, RC_VERDICT_FAIL = 2 };

typedef struct { uint32_t subject_kind; uint64_t subject; uint32_t verdict; } RcEvidenceRecord;

/* What rules are checked against. The evidence registry is append-only and
 * is filled by the evidence gate when it publishes. */
typedef struct {
    RxWorld *w;
    uint32_t principal;
    const AgCapTable *caps;             /* what the principal holds (read only) */
    uint64_t next_seq;
    uint64_t active_generation, authority_epoch;
    RxObjRef refs[RC_MAX_REFS];         /* context objects rules may refer to */
    uint64_t values[4];                 /* context values rules may refer to */
    const AgSkillTable *skills;
    uint32_t skill_arity[AG_MAX_SKILLS];
    uint32_t n_effect_res;
    uint64_t effect_res[RC_MAX_EFFECT_RES];
    uint64_t forbid_lo, forbid_hi;      /* hi 0 = nothing forbidden */
    uint32_t effects_used, max_effects;
    uint32_t max_graph_nodes;
    pthread_mutex_t mu;                 /* guards the evidence registry and effects_used */
    uint32_t n_evidence;
    RcEvidenceRecord evidence[RC_MAX_EVIDENCE];
} RcContext;

void rc_context_init(RcContext *ctx, RxWorld *w, uint32_t principal, const AgCapTable *caps);
void rc_context_destroy(RcContext *ctx);
int  rc_context_add_evidence(RcContext *ctx, uint32_t subject_kind, uint64_t subject,
                             uint32_t verdict);

/* ---- contracts ---- */

/* The standard contract of `kind`; fills its identity. */
int  rc_contract_std(RcKind kind, RcContract *out);
void rc_contract_identify(RcContract *c);
const char *rc_kind_name(RcKind k);
const char *rc_rule_kind_name(RcRuleKind k);

/* ---- checking ---- */

#define RC_TYPE_RULE 0xffffu    /* violation of a field type, not of a rule */

typedef struct {
    int ok;
    uint32_t n;                         /* violations */
    struct { uint16_t rule; uint8_t field; uint8_t kind; } v[RC_MAX_RULES + RC_MAX_FIELDS];
    uint32_t kinds_violated;            /* bit per RcRuleKind; bit 0 = a type */
} RcVerdict;

/* Every type and every rule, against the context. Pure: changes nothing. */
int rc_check(const RcContract *c, const RcContext *ctx, const RcObject *o, RcVerdict *v);

/* Apply the repairs the violated rules allow. Returns the number of fields
 * changed, or -1 when a violation has no repair (the object must be
 * rejected). Only derived fields and narrowing are ever touched. */
int rc_repair(const RcContract *c, const RcContext *ctx, RcObject *o, const RcVerdict *v);

/* ---- compiling for a constrained backend ---- */

typedef struct {
    uint64_t lo, hi;
    uint64_t bits;              /* allowed bits (is_bits), else allowed values < 64 (has_enum) */
    uint8_t is_bits, has_enum, has_set, empty;
    uint32_t n_set;
    uint64_t set[RC_MAX_SET];   /* explicit allowed values (has_set) */
    uint64_t ne[RC_MAX_RULES];  /* values excluded */
    uint32_t n_ne;
    uint8_t has_x;
    uint64_t xlo, xhi;          /* an excluded interval (has_x) */
} RcDomain;

typedef struct {
    const RcContract *c;
    uint8_t pos[RC_MAX_FIELDS];         /* position of each field in the order */
    uint8_t enforced[RC_MAX_RULES];     /* 1: folded into a domain; 0: checked after */
    uint32_t n_enforced, n_post;
} RcCompiled;

/* Decide which rules become per-field domains. Context-free. */
int rc_compile(const RcContract *c, RcCompiled *out);

/* Allowed values of `field`, given the fields before it in the order
 * (partial->f) and the context. */
int rc_domain(const RcCompiled *p, const RcContext *ctx, const RcObject *partial,
              uint32_t field, RcDomain *d);
int rc_domain_contains(const RcDomain *d, uint64_t v);
/* The allowed value nearest to v (a constrained decoder's fallback). */
int rc_domain_pick(const RcDomain *d, uint64_t v, uint64_t *out);

/* ---- producing ---- */

/* A cognitive backend. draw proposes one field value; when the backend is
 * constrained, `dom` is the compiled domain and the value must lie in it.
 * graph returns the graph an ACTION_GRAPH candidate attaches. */
typedef struct {
    const char *name;
    int constrained;
    uint64_t (*draw)(void *self, const RcContract *c, uint32_t attempt, uint32_t field,
                     const RcObject *partial, const RcDomain *dom);
    const AgGraph *(*graph)(void *self, uint32_t attempt);
    void (*begin)(void *self, uint32_t attempt);
    void *self;
} RcBackend;

enum { RC_OUT_VALID = 1,        /* first candidate was valid */
       RC_OUT_REPAIRED,         /* valid after repair */
       RC_OUT_RETRIED,          /* valid after at least one rejected candidate */
       RC_OUT_REJECTED,         /* retries exhausted: nothing to publish */
       RC_OUT_UNSATISFIABLE };  /* an empty domain: nothing could be valid; nothing generated past it */

typedef struct {
    uint32_t candidates;        /* complete candidates generated */
    uint32_t invalid;           /* of which failed the contract as generated */
    uint32_t repairs;           /* candidates repaired into validity */
    uint32_t retries;
    uint32_t dead_ends;         /* constrained: a later domain was empty (no lookahead) */
    uint32_t draws;             /* field values drawn from the backend (compute) */
    uint32_t checks;            /* rc_check calls */
    uint32_t kinds_violated;    /* union over invalid candidates */
    uint64_t gen_ns, check_ns;
} RcStats;

/* Generate -> check -> repair/reject -> retry, up to max_retries extra
 * candidates. On RC_OUT_VALID/REPAIRED/RETRIED *out holds a candidate
 * that passed rc_check; otherwise *out must not be published. */
int rc_produce(const RcCompiled *p, const RcContext *ctx, const RcBackend *b,
               uint32_t max_retries, RcObject *out, RcStats *st);

/* ---- serialization ---- */

/* Canonical binary: kind (u32 LE), contract identity (32), fields (u64 LE). */
size_t rc_encode(const RcContract *c, const RcObject *o, uint8_t *buf, size_t cap);
int    rc_decode(const RcContract *c, const uint8_t *buf, size_t n, RcObject *o);
/* Object identity: SHA-256 of the canonical binary (graph digest folded in). */
void   rc_identity(const RcContract *c, const RcObject *o, uint8_t out[32]);
/* One JSON object: {"kind":"<name>","<field>":<uint>,...}. */
int    rc_to_json(const RcContract *c, const RcObject *o, char *buf, size_t cap);
/* Strict: exact keys, unsigned decimal integers, no duplicates, nothing else.
 * Returns 0, or negative for a text that is not this contract's object. It
 * does not check rules; rc_check does. */
int    rc_from_json(const RcContract *c, const char *text, RcObject *o);

/* ---- the publish gate (a resident reaction) ---- */

#define RC_OT_DRAFT   0x5C00u   /* + kind */
#define RC_OT_STATUS  0x5C40u   /* + kind */
#define RC_OT_PUBLISH 0x5C80u   /* + kind (default world_type) */

/* status: 0 drafts seen, 1 last verdict (RC_GATE_*), 2 first violated rule
 *         (RC_TYPE_RULE for a type), 3 violated kinds, 4 published,
 *         5 rejected, 6 repaired, 7 last draft digest word */
enum { RC_GATE_PUBLISHED = 1, RC_GATE_REPAIRED, RC_GATE_REJECTED };

typedef struct {
    const RcContract *c;
    RcContext *ctx;
    RxObjRef draft, published, status;
    uint32_t reaction;
    uint32_t subject;
    const AgGraph *volatile graph;      /* graph attached to the current ACTION_GRAPH draft */
} RcGate;

/* Register the gate. subject: the gate's principal; caps: R on draft, RW on
 * published and status (published must be writable by the gate only). */
int rc_gate_register(RcGate *g, RxWorld *w, const RcContract *c, RcContext *ctx,
                     uint32_t subject, RxObjRef draft, RxCapRef draft_read,
                     RxObjRef published, RxCapRef published_rw,
                     RxObjRef status, RxCapRef status_rw);

#endif /* RX_CONTRACT_H */
