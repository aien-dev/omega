/*
 * rx_projection.h -- Omega cognitive state projection IR (host reference).
 *
 * Cognition does not get "everything we know". It states what one operation
 * needs (CognitiveNeed). Omega turns that into the smallest state that is
 * enough for the operation (StateProjection), by typed queries to Cortex.
 * Nothing here is text or tokens. A projection is object references, typed
 * summaries and derived numbers.
 *
 * "Compile" here means selecting and shaping state from a declared need. It
 * generates no code.
 *
 * Every projected entry has a treatment (PjStateClass). Treatment is
 * semantic. It says what the operation may do with the state, not how it is
 * laid out:
 *
 *   PINNED             always present, in full; never dropped for budget
 *   LIVE               the current value, in full
 *   DERIVED            replaced by one computed feature; sources recoverable
 *   RECALLABLE         a reference; the body can be recalled on demand
 *   COMPRESSIBLE       lossy summary allowed; source recoverable
 *   REFERENCE_ONLY     identity, header and digest only
 *   EPHEMERAL          valid for this operation only; not part of the
 *                      recoverability contract; never protected state
 *   BRANCH_LOCAL       exists only on the need's hypothetical branch
 *   PROTECTED_EVIDENCE never summarized, never derived, never budget-dropped
 *
 * Protected state (rx_cortex.h CX_PROT_*) is authority, commit receipts,
 * verification evidence, generation identity and effect receipts. A need
 * that asks to summarize or derive it does not compile: the compiler refuses
 * the whole projection (PJ_ERR_PROTECTED). Protected state may appear in
 * full, or as a reference that keeps its exact digest, never as a summary.
 *
 * Excluded state is not copied anywhere. The projection records the Cortex
 * chain head it was compiled against and counts of what it left out; the
 * audit (pj_audit) re-derives, object by object, why each excluded object
 * was left out and checks it is still intact in Cortex.
 */
#ifndef RX_PROJECTION_H
#define RX_PROJECTION_H

#include "rx_cortex.h"

#include <stdint.h>

typedef enum {
    PJ_PINNED = 1, PJ_LIVE, PJ_DERIVED, PJ_RECALLABLE, PJ_COMPRESSIBLE,
    PJ_REFERENCE_ONLY, PJ_EPHEMERAL, PJ_BRANCH_LOCAL, PJ_PROTECTED_EVIDENCE,
    PJ_STATE_CLASS_END
} PjStateClass;

typedef enum { PJ_FORM_FULL = 1, PJ_FORM_SUMMARY, PJ_FORM_REF } PjForm;

typedef enum { PJ_OP_EXPLAIN = 1, PJ_OP_VERIFY, PJ_OP_PLAN, PJ_OP_ASSESS, PJ_OP_PREDICT } PjOperation;

/* Which part of a subject's history one fact type draws from. */
typedef enum {
    PJ_T_WINDOW = 1,         /* t in the need's window */
    PJ_T_LATEST,             /* the newest at or before now */
    PJ_T_CARRY_IN,           /* the newest before the window, plus the window */
    PJ_T_ALL,                /* the whole history up to now */
    PJ_T_MEMORY              /* WINDOW or ALL, from the need's memory scope */
} PjTemporal;

/* Where the need's window starts. */
typedef enum {
    PJ_A_FIXED = 1,          /* temporal.t0 */
    PJ_A_SINCE_VERIFIED,     /* the subject's newest verified realization */
    PJ_A_GENERATION          /* the start of the current generation */
} PjAnchor;

typedef enum {
    PJ_W_SELF = 1,           /* the subject */
    PJ_W_DEPENDENCIES,       /* the subject and what it depends on (one hop) */
    PJ_W_MACHINE             /* subject 0: machine-wide facts (generations) */
} PjWorld;

typedef enum { PJ_U_NONE = 1, PJ_U_POINT, PJ_U_INTERVAL } PjUncertainty;
typedef enum { PJ_E_NONE = 1, PJ_E_REFS, PJ_E_FULL } PjEvidence;
typedef enum { PJ_M_WORKING = 1, PJ_M_EPISODIC } PjMemory;

/* Sections of a projection. */
typedef enum {
    PJ_SEC_WORLD = 1,        /* canonical_world_refs: current (LIVE, LATEST) state */
    PJ_SEC_CORTEX,           /* cortex_refs */
    PJ_SEC_HYPOTHESIS,       /* hypotheses: claims */
    PJ_SEC_EVIDENCE          /* evidence_refs: evidence and protected state */
} PjSection;

/* Verification evidence carries this tag when the check passed. */
#define PJ_VERIFIED 1u

#define PJ_MAX_FACTS 16u

typedef struct {
    uint32_t cls, kind;      /* Cortex class and kind */
    uint32_t treatment;      /* PjStateClass */
    uint32_t world;          /* PjWorld */
    uint32_t temporal;       /* PjTemporal */
} PjFactReq;

typedef struct {
    uint32_t goal;               /* the caller's goal code (opaque to Omega) */
    uint32_t operation_class;    /* PjOperation */
    uint64_t subject;
    uint64_t focus;              /* one object the operation is about (0 = none); pinned */
    uint32_t branch;             /* 0 = the main line */
    PjFactReq required_fact_types[PJ_MAX_FACTS];
    uint32_t n_fact_types;
    struct {
        uint32_t anchor;         /* PjAnchor */
        uint64_t t0;             /* PJ_A_FIXED */
        uint64_t t_now;
        uint32_t realization_kind;   /* PJ_A_SINCE_VERIFIED: which realizations */
        uint32_t generation_kind;    /* PJ_A_GENERATION: machine entity kind */
    } required_temporal_scope;
    struct {
        uint32_t dependency_kind;    /* relationship kind naming a dependency */
    } required_world_scope;
    uint32_t uncertainty_requirement;    /* PjUncertainty */
    uint32_t evidence_requirement;       /* PjEvidence */
    uint32_t memory_scope;               /* PjMemory */
    struct {
        uint64_t max_bytes, max_objects;    /* 0 = unbounded */
    } resource_budget;
} CognitiveNeed;

typedef struct {
    uint64_t id;
    uint32_t section, treatment, form;
    uint32_t n;              /* summary words (PJ_FORM_SUMMARY) */
    uint64_t off;            /* in the projection arena */
    uint8_t digest[32];      /* the source object's digest */
} PjEntry;

/* A number computed from many Cortex objects (DERIVED). */
typedef struct {
    uint32_t section;        /* PJ_FEAT_DERIVED or PJ_FEAT_UNCERTAINTY */
    uint32_t cls, kind, branch;
    uint64_t subject, t0, t1;
    uint64_t n_src, count, sum, sumsq, min, max;
    uint8_t src_digest[32];  /* over the sources' digests, in order */
} PjFeature;

enum { PJ_FEAT_DERIVED = 1, PJ_FEAT_UNCERTAINTY };

typedef struct {
    uint64_t subject, t0, t1;
    uint32_t anchor;
    uint64_t anchor_id;      /* the verified realization or generation object */
    uint32_t treatment;      /* always PJ_EPHEMERAL: compile-time scaffolding */
} PjWindow;

enum { PJ_X_OTHER_SUBJECTS = 1, PJ_X_IN_SCOPE_SUBJECT };

typedef struct {
    uint32_t reason;         /* PJ_X_* */
    uint64_t subject;        /* PJ_X_IN_SCOPE_SUBJECT */
    uint64_t count;
} PjExclusion;

/* Per-object reasons, from the audit. */
typedef enum {
    PJ_WHY_INCLUDED = 0, PJ_WHY_SUBJECT = 1, PJ_WHY_TYPE, PJ_WHY_TIME, PJ_WHY_BRANCH,
    PJ_WHY_SUPERSEDED, PJ_WHY_DERIVED_SOURCE, PJ_WHY_END
} PjWhy;

typedef struct {
    uint64_t id;
    uint32_t form;           /* PJ_FORM_SUMMARY or PJ_FORM_REF */
    uint8_t digest[32];
} PjRecover;

typedef struct {
    PjEntry *e;
    uint32_t n, cap;
    PjFeature f[PJ_MAX_FACTS * 4];
    uint32_t nf;
    PjWindow w[8];
    uint32_t nw;
    PjExclusion x[72];
    uint32_t nx;
    PjRecover *r;            /* recoverability manifest */
    uint32_t nr, rcap;
    uint64_t *arena;
    uint64_t arena_n, arena_cap;
    uint64_t store_count;    /* Cortex objects at compile time */
    uint8_t store_chain[32]; /* Cortex chain head at compile time */
    /* measurements */
    uint64_t bytes;          /* what cognition receives */
    uint64_t objects;        /* entries plus features */
    uint64_t examined;       /* Cortex objects the queries touched */
    uint64_t compile_ns;
} StateProjection;

enum {
    PJ_OK = 0, PJ_ERR_ARG = -1, PJ_ERR_NOMEM = -2,
    PJ_ERR_PROTECTED = -3,   /* a need asked to summarize or derive protected state */
    PJ_ERR_BUDGET = -4,      /* the pinned and protected state alone exceeds the budget */
    PJ_ERR_NO_ANCHOR = -5,   /* no verified realization / generation to anchor the window */
    PJ_ERR_BRANCH = -6,      /* BRANCH_LOCAL without a branch */
    PJ_ERR_RECOVER = -7      /* recovery found a source that no longer matches */
};

/* Bytes cognition is charged per entry header, per payload word, per digest. */
#define PJ_HEADER_BYTES 96u
#define PJ_WORD_BYTES 8u
#define PJ_DIGEST_BYTES 32u
#define PJ_FEATURE_BYTES 96u
/* The sample-series summary: count, sum, sum of squares, min, max. */
#define PJ_SUMMARY_WORDS 5u

void pj_need_init(CognitiveNeed *n, uint32_t operation, uint64_t subject, uint64_t t_now);
int  pj_need_add(CognitiveNeed *n, uint32_t cls, uint32_t kind, uint32_t treatment,
                 uint32_t world, uint32_t temporal);

int  pj_compile(CxStore *s, const CognitiveNeed *need, StateProjection *out);
void pj_free(StateProjection *p);

/* ---- what cognition consumes ----
 *
 * One flat view for both a projection and the whole store, so the same
 * consumer runs on either. Items are sorted by id. */
typedef struct {
    uint64_t id;
    uint32_t cls, kind;
    uint64_t subject, t, generation, tag;
    uint64_t links[CX_LINKS];
    uint32_t branch, protect;
    uint32_t section, treatment, form;
    uint32_t n;
    const uint64_t *w;       /* payload (FULL) or summary words (SUMMARY) */
} PjItem;

typedef struct {
    PjItem *items;
    uint32_t n;
    const PjFeature *feat;
    uint32_t nf;
    int dense;               /* items[i].id == i + 1 (the whole store) */
    uint64_t bytes;
    uint64_t build_ns;
} PjView;

int  pj_view_projection(const CxStore *s, const StateProjection *p, PjView *v);
int  pj_view_full(const CxStore *s, PjView *v);
/* Arbitrary objects in full form (for baselines such as "the newest K"). */
int  pj_view_ids(const CxStore *s, const uint64_t *ids, uint32_t n, PjView *v);
void pj_view_free(PjView *v);
const PjItem *pj_view_find(const PjView *v, uint64_t id);

/* Bytes of the whole store under the same accounting as a projection. */
uint64_t pj_full_bytes(const CxStore *s);

/* Recall one RECALLABLE or REFERENCE_ONLY body; the digest is checked first. */
const uint64_t *pj_recall(const CxStore *s, const StateProjection *p, uint64_t id, uint32_t *n_out);

/* The feature from its sources: recompute count, sums, extremes and digest. */
int  pj_feature_compute(CxStore *s, uint64_t subject, uint32_t cls, uint32_t kind, uint32_t branch,
                        uint64_t t0, uint64_t t1, PjFeature *out);

/* Summary of one sample series (count, sum, sumsq, min, max). */
void pj_summarize(const uint64_t *w, uint32_t n, uint64_t out[PJ_SUMMARY_WORDS]);

/* ---- recoverability ----
 *
 * pj_recover_all checks every manifest entry and every derived feature
 * against Cortex: sources present, digests unchanged, features recompute to
 * the same numbers. Returns PJ_OK or PJ_ERR_RECOVER; counts in *checked. */
int  pj_recover_all(CxStore *s, const StateProjection *p, uint64_t *checked);

typedef struct {
    uint64_t included, excluded;
    uint64_t by_why[PJ_WHY_END];
    uint64_t excluded_intact;   /* excluded objects whose digest still verifies */
    uint64_t overlap;           /* objects both included and excluded (must be 0) */
    uint64_t other_branch_included;  /* must be 0 */
    int counts_match;           /* excluded_state counts agree with the audit */
} PjAudit;

/* Classify every Cortex object as included or excluded, with one reason.
 * O(store); it is an audit, not part of the cognition path. */
int  pj_audit(CxStore *s, const CognitiveNeed *need, const StateProjection *p, PjAudit *a);

const char *pj_state_class_name(uint32_t c);

#endif /* RX_PROJECTION_H */
