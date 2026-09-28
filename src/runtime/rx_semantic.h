/*
 * rx_semantic.h -- Omega semantic variables and incremental recomputation.
 *
 * Objective: do not recompute cognition or derived state when nothing it
 * depends on changed.
 *
 * A semantic value names one piece of meaning at field granularity: one field
 * of one world object (a source), or the result of one pure derivation over
 * named inputs (a derived value). A derived value is reused when
 *
 *     same derivation  +  same semantic inputs  +  same relevant generations
 *
 * and recomputed otherwise. "Same semantic inputs" is content, not position:
 * two inputs are the same when their semantic id, their generation and the
 * digest of their value bytes are all equal. Nothing else participates.
 *
 * Three layers, from cheapest to most general:
 *
 *   1. Branch memo. Each branch keeps the last value of every derived node it
 *      has asked for. A node is CLEAN until one of its declared inputs
 *      changes in that branch; then it and everything downstream of it is
 *      marked DIRTY (field-granular, transitive). A CLEAN node is served with
 *      no work at all. An unrelated field or object never marks it.
 *   2. Early cutoff. A DIRTY node first brings its inputs up to date and
 *      recomputes its content key. If the key equals the one its memo was
 *      built from, the memo is still exact and nothing runs.
 *   3. Content-addressed cache. The key (derivation id, and for each input
 *      its semantic id, generation and value digest) addresses one immutable
 *      entry shared by every branch, agent, task and J-Space candidate in the
 *      engine. A hit is exact reuse of a result computed elsewhere from the
 *      same inputs. A miss runs the function once and files the result.
 *
 * Generations. A source value carries its world object's generation;
 * retiring an object and creating another in its slot changes it, so no
 * result derived from the old object is served for the new one even when
 * the bytes match. A derivation carries its implementation generation (the
 * R9 generation that put the function in force); promoting a new
 * implementation gives a new derivation id, so results of the old one are
 * not served for it.
 *
 * Branches. A branch is a forked view of the semantic world: its own source
 * values and its own memo. A J-Space candidate is a branch with a candidate
 * id. Writes in one branch never change another branch's values, memo or
 * dirty state. The cache is shared, and is safe to share because its keys
 * are the complete content of what a result was derived from.
 *
 * Provenance. Every cache entry records who produced it (branch, agent,
 * task, candidate), from exactly which inputs (with each source input's
 * world causal crumb), and a digest over all of that. Every reuse appends a
 * hash-chained REUSE event naming the entry it served and who consumed it.
 * rx_sem_verify recomputes every digest and link.
 *
 * Functions must be pure over their declared inputs. The engine cannot see
 * a function read something it did not declare; the audit mode (recompute
 * on every hit and compare) exists to catch that, and the qualification
 * runs with it on.
 *
 * This is the simple human reference: one engine mutex, bounded fixed-size
 * tables, a fork copies the branch's tables. It is not optimized.
 */
#ifndef RX_SEMANTIC_H
#define RX_SEMANTIC_H

#include "rx_world.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define RX_SEM_MAX_SOURCES     256u
#define RX_SEM_MAX_NODES       256u
#define RX_SEM_MAX_FNS         32u
#define RX_SEM_MAX_INPUTS      8u
#define RX_SEM_MAX_BRANCHES    64u
#define RX_SEM_MAX_EVIDENCE    4u
#define RX_SEM_VALUE_WORDS     8u
#define RX_SEM_CACHE_SLOTS     4096u   /* power of two */
#define RX_SEM_CONFIDENCE_ONE  1000000u /* confidence is parts per million */

#define RX_SEM_OK               0
#define RX_SEM_ERR_ARG        -60
#define RX_SEM_ERR_FULL       -61
#define RX_SEM_ERR_NOT_FOUND  -62
#define RX_SEM_ERR_STALE      -63   /* a source's world object was retired */
#define RX_SEM_ERR_FN         -64   /* the derivation function failed */
#define RX_SEM_ERR_CYCLE      -65
#define RX_SEM_ERR_EXISTS     -66
#define RX_SEM_ERR_VERIFY     -67

/* Semantic ids. The top byte is the kind; the rest identifies. */
#define RX_SEM_KIND_SOURCE   0x01u
#define RX_SEM_KIND_DERIVED  0x02u
#define RX_SEM_KIND(id)      ((uint32_t)((id) >> 56))

/* Source id of one field of one world object slot. Stable across that
 * slot's generations: the generation travels with the value instead. */
#define RX_SEM_SOURCE_ID(obj_id, field) \
    (((uint64_t)RX_SEM_KIND_SOURCE << 56) | ((uint64_t)(obj_id) << 8) | (uint64_t)(field))

typedef struct { uint8_t b[32]; } RxSemDigest;

/* SemanticValue. */
typedef struct {
    uint64_t semantic_id;
    uint32_t type;
    uint64_t value_version;      /* per branch; moves only when the content changes */
    uint32_t n_deps;
    uint64_t dependencies[RX_SEM_MAX_INPUTS];  /* semantic ids it was derived from */
    uint32_t generation;         /* source: world object generation; derived: fn generation */
    RxSemDigest derivation_id;   /* derived: function identity; source: zero */
    uint32_t confidence;         /* ppm */
    uint32_t n_evidence;
    RxSemDigest evidence_refs[RX_SEM_MAX_EVIDENCE];
    uint32_t n_words;
    uint64_t value[RX_SEM_VALUE_WORDS];
    RxSemDigest value_digest;    /* content address of (type, value words) */
} RxSemValue;

/* One input as a derivation saw it. The first three fields are the key. */
typedef struct {
    uint64_t semantic_id;
    uint32_t generation;
    RxSemDigest value_digest;
    uint64_t value_version;      /* provenance only: versions are branch-local */
    RxSemDigest evidence;        /* source: the world crumb that wrote it */
} RxSemInputVersion;

/* DerivedSemanticValue: one immutable cache entry. */
typedef struct {
    bool used;
    uint32_t function_id;
    RxSemDigest derivation_id;
    RxSemDigest key;
    uint32_t n_inputs;
    RxSemInputVersion input_versions[RX_SEM_MAX_INPUTS];
    RxSemValue result;
    /* Provenance of the one computation that produced it. */
    uint32_t producer_branch;
    uint32_t producer_agent;
    uint64_t producer_task;
    uint64_t producer_candidate;
    uint64_t compute_ns;         /* measured wall time of that computation */
    uint64_t seq;                /* order of production in this engine */
    RxSemDigest entry_digest;    /* over everything above */
    uint64_t reuses;
} RxSemDerived;

/* A derivation function: read `in[0..n_in)`, fill out->value / n_words /
 * confidence. Return 0 on success. It must depend on nothing else. */
typedef int (*RxSemFn)(const RxSemValue *const *in, uint32_t n_in, RxSemValue *out, void *user);

typedef struct {
    const char *name;
    uint32_t out_type;
    uint32_t generation;         /* implementation generation (R9) */
    RxSemDigest impl_digest;     /* identity of the implementation bytes */
    RxSemFn fn;
    void *user;
} RxSemFnDesc;

/* Per-branch, per-node memo state. */
enum { RX_SEM_ABSENT = 0, RX_SEM_CLEAN, RX_SEM_DIRTY };

/* How a get was answered. */
typedef enum {
    RX_SEM_SERVED_MEMO = 1,      /* clean memo: no work */
    RX_SEM_SERVED_CUTOFF,        /* dirty, but the content key was unchanged */
    RX_SEM_SERVED_CACHE,         /* key found in the shared cache */
    RX_SEM_SERVED_COMPUTED       /* the function ran */
} RxSemServed;

typedef struct {
    uint64_t gets;
    uint64_t memo_hits;
    uint64_t cutoffs;            /* dirty nodes whose inputs came back unchanged */
    uint64_t cache_hits;
    uint64_t cache_hits_cross_branch;
    uint64_t cache_hits_cross_agent;
    uint64_t cache_hits_cross_task;
    uint64_t cache_hits_cross_candidate;
    uint64_t computes;
    uint64_t compute_ns;         /* time spent in functions that ran */
    uint64_t saved_ns;           /* producers' measured time for every avoided run */
    uint64_t invalidations;      /* memo entries marked DIRTY */
    uint64_t source_writes;
    uint64_t source_writes_unchanged; /* same content: nothing invalidated */
    uint64_t audits;             /* hits recomputed in audit mode */
    uint64_t false_hits;         /* audit recompute differed from the served value */
    uint64_t cache_full;         /* results not filed because the cache was full */
} RxSemStats;

typedef struct {
    uint64_t id;                 /* 1-based */
    uint32_t kind;               /* 1 = PRODUCE, 2 = REUSE */
    uint32_t entry;              /* cache slot */
    RxSemDigest entry_digest;
    uint32_t branch;
    uint32_t agent;
    uint64_t task;
    uint64_t candidate;
    /* The consumer's own input evidence (its world crumbs). A reuse keeps
     * the producer's provenance in the entry and adds the consumer's here. */
    RxSemDigest inputs_evidence;
    RxSemDigest prev;
    RxSemDigest digest;          /* H(prev, this event) */
} RxSemEvent;

enum { RX_SEM_EVENT_PRODUCE = 1, RX_SEM_EVENT_REUSE = 2 };

typedef struct {
    bool used;
    uint64_t semantic_id;
    uint32_t function_id;
    uint32_t n_inputs;
    uint64_t inputs[RX_SEM_MAX_INPUTS];
    /* Reverse index: nodes that read this node. */
    uint32_t n_readers;
    uint32_t readers[RX_SEM_MAX_NODES];
} RxSemNode;

typedef struct {
    bool used;
    uint64_t semantic_id;
    uint32_t type;
    bool world_bound;
    RxObjRef obj;                /* bound world slot and generation */
    uint32_t field;
    uint32_t n_readers;
    uint32_t readers[RX_SEM_MAX_NODES];
    /* Object-granular mode only: every field of this object, for the baseline. */
} RxSemSource;

typedef struct {
    bool live;
    uint32_t id;
    uint32_t parent;             /* UINT32_MAX for a root */
    uint32_t agent;
    uint64_t task;
    uint64_t candidate;          /* J-Space candidate id; 0 = none */
    uint64_t version_clock;
    bool have_source[RX_SEM_MAX_SOURCES];
    bool source_stale[RX_SEM_MAX_SOURCES];
    RxSemValue source[RX_SEM_MAX_SOURCES];
    uint64_t world_version[RX_SEM_MAX_SOURCES];  /* field version last pulled */
    uint8_t state[RX_SEM_MAX_NODES];
    RxSemValue memo[RX_SEM_MAX_NODES];
    RxSemDigest memo_key[RX_SEM_MAX_NODES];
    uint32_t memo_entry[RX_SEM_MAX_NODES];  /* cache slot it came from; UINT32_MAX if none */
    uint64_t invalidations;
} RxSemBranch;

/* Invalidation granularity. FIELD is the engine. OBJECT is the measured
 * baseline: a write to any field of an object dirties every reader of any
 * field of that object. */
enum { RX_SEM_GRAIN_FIELD = 0, RX_SEM_GRAIN_OBJECT = 1 };

typedef struct RxSemEngine {
    pthread_mutex_t mu;
    uint32_t grain;
    bool cache_enabled;          /* false: memo + cutoff only (baseline) */
    bool audit;                  /* recompute every hit and compare */

    RxSemFnDesc fns[RX_SEM_MAX_FNS];
    RxSemDigest fn_derivation[RX_SEM_MAX_FNS];
    uint32_t n_fns;

    RxSemSource sources[RX_SEM_MAX_SOURCES];
    uint32_t n_sources;
    RxSemNode nodes[RX_SEM_MAX_NODES];
    uint32_t n_nodes;

    RxSemBranch *branches;       /* RX_SEM_MAX_BRANCHES */
    RxSemDerived *cache;         /* RX_SEM_CACHE_SLOTS */
    uint64_t cache_seq;
    uint32_t cache_used;

    RxSemEvent *events;
    uint64_t n_events;
    uint64_t event_cap;
    uint64_t event_overflow;

    RxSemStats stats;
} RxSemEngine;

int  rx_sem_init(RxSemEngine *e, uint64_t event_cap);
void rx_sem_destroy(RxSemEngine *e);
void rx_sem_set_mode(RxSemEngine *e, uint32_t grain, bool cache_enabled, bool audit);

/* Digest helpers. */
void rx_sem_digest_value(uint32_t type, const uint64_t *words, uint32_t n, RxSemDigest *out);
void rx_sem_digest_impl(const char *text, RxSemDigest *out);
bool rx_sem_digest_eq(const RxSemDigest *a, const RxSemDigest *b);

/* Register a derivation function. Its derivation id is
 * H(name, out_type, generation, impl_digest). */
int  rx_sem_add_fn(RxSemEngine *e, const RxSemFnDesc *d, uint32_t *out_fn);

/* Declare a source: one field of a world object slot, or (obj.id, field)
 * with world_bound false for a value that only branches set. */
int  rx_sem_add_source(RxSemEngine *e, RxObjRef obj, uint32_t field, uint32_t type,
                       bool world_bound, uint64_t *out_sid);
/* Declare a derived node: `fn` over the listed semantic ids (sources or
 * earlier nodes). Its semantic id is structural (function and inputs), so
 * the same question has the same id in every branch. */
int  rx_sem_add_node(RxSemEngine *e, uint32_t fn, const uint64_t *inputs, uint32_t n,
                     uint64_t *out_sid);

/* Branches. A root has no parent. A fork copies the parent's sources and
 * memo as they are now; afterwards they are independent. */
int  rx_sem_branch_root(RxSemEngine *e, uint32_t agent, uint64_t task, uint32_t *out);
int  rx_sem_branch_fork(RxSemEngine *e, uint32_t parent, uint32_t agent, uint64_t task,
                        uint64_t candidate, uint32_t *out);
int  rx_sem_branch_drop(RxSemEngine *e, uint32_t branch);
/* The branch's context (who is asking). Does not change any value. */
int  rx_sem_branch_context(RxSemEngine *e, uint32_t branch, uint32_t agent, uint64_t task,
                           uint64_t candidate);

/* Write a source value in one branch. Same content (and generation): no
 * version change, nothing invalidated. Otherwise the value version moves and
 * the readers are marked DIRTY, transitively. `evidence` may be null. */
int  rx_sem_set_source(RxSemEngine *e, uint32_t branch, uint64_t sid, uint32_t generation,
                       const uint64_t *words, uint32_t n, uint32_t confidence,
                       const RxSemDigest *evidence);

/* Pull every world-bound source from the world into one branch. A field whose
 * world version did not move is not touched. A source whose object was
 * retired becomes stale: nodes reading it fail with RX_SEM_ERR_STALE until
 * rx_sem_rebind names the new object. Returns the number of sources that
 * changed, or a negative error. */
int  rx_sem_sync_world(RxSemEngine *e, uint32_t branch, RxWorld *w);
int  rx_sem_rebind(RxSemEngine *e, uint64_t sid, RxObjRef obj);

/* Demand a derived value in a branch. `served` (may be null) says how. */
int  rx_sem_get(RxSemEngine *e, uint32_t branch, uint64_t sid, RxSemValue *out,
                RxSemServed *served);
/* Read a source value as the branch holds it. */
int  rx_sem_read_source(RxSemEngine *e, uint32_t branch, uint64_t sid, RxSemValue *out);

/* Memo state of a node in a branch (RX_SEM_ABSENT/CLEAN/DIRTY). */
int  rx_sem_state(RxSemEngine *e, uint32_t branch, uint64_t sid);

/* The cache entry a clean derived value in this branch was taken from. */
int  rx_sem_explain(RxSemEngine *e, uint32_t branch, uint64_t sid, RxSemDerived *out);

/* Recompute every entry digest, every event link, and check that each clean
 * memo in each live branch is exactly its entry's result and that the
 * entry's inputs are what the branch holds now. 0 on success. */
int  rx_sem_verify(RxSemEngine *e, uint64_t *entries_checked, uint64_t *events_checked,
                   uint64_t *memos_checked);

/* Declared structure, for dependency checks: the node's inputs and the
 * transitive set of nodes a write to `sid` would dirty (field grain). */
int  rx_sem_node_inputs(RxSemEngine *e, uint64_t sid, uint64_t *inputs, uint32_t *n);
int  rx_sem_downstream(RxSemEngine *e, uint64_t sid, uint64_t *out, uint32_t cap, uint32_t *n);

#endif
