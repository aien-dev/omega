/*
 * rx_jspace.h -- branch-native shared cognitive state for J-Space.
 *
 * Two layers, kept apart on purpose.
 *
 * Semantic layer. A branch is an ordered sequence of state units. Each unit has
 * a semantic identity: a SHA-256 chain over the semantic events that produced it
 * (root seed, a derive step with its input token, an edit with its offset and
 * patch). Identity never mentions a realization type, bytes, placement or
 * residency. Two branches that took the same semantic steps hold the same
 * semantic identities, whatever their physical state.
 *
 * Physical layer. A realization is one way the bytes of one semantic unit are
 * held right now: a native latent checkpoint, a neural activation checkpoint,
 * attention/KV state, compiled features, a World or Cortex projection, or a
 * physical tensor. A realization records its recipe (how to rebuild it from its
 * parent realization), its placement and its measured costs. Branches share
 * realizations by reference; a realization referenced by more than one holder
 * is immutable. Writing through a branch to a shared realization copies it (or
 * rebuilds it) into a private one first. A branch that has children is frozen.
 *
 * The FORGE branch policy chooses reuse, copy, reference, recompute, move,
 * compress, spill or evict by comparing measured costs. It never changes a
 * semantic identity and it never changes the bytes a branch reads.
 *
 * Realizers are a vtable. Nothing in the store or the policy depends on which
 * representation a realizer uses.
 */
#ifndef RX_JSPACE_H
#define RX_JSPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JS_OK             0
#define JS_ERR_ARG       -1
#define JS_ERR_FULL      -2
#define JS_ERR_FROZEN    -3   /* branch has children: its state is a shared ancestor */
#define JS_ERR_IO        -4
#define JS_ERR_CORRUPT   -5   /* restored bytes disagree with the recorded content digest */
#define JS_ERR_NOMEM     -6

typedef struct { uint8_t b[32]; } JsSemId;

/* Realization types. Listed so receipts can name them; none is privileged. */
typedef enum {
    JS_REAL_LATENT_CHECKPOINT = 1,
    JS_REAL_ACTIVATION_CHECKPOINT,
    JS_REAL_KV_STATE,
    JS_REAL_COMPILED_FEATURES,
    JS_REAL_WORLD_PROJECTION,
    JS_REAL_CORTEX_PROJECTION,
    JS_REAL_PHYSICAL_TENSOR
} JsRealType;

/* Where the bytes are. Placement is not identity. */
typedef enum {
    JS_PLACE_HOT = 1,       /* host arena A, uncompressed */
    JS_PLACE_COLD,          /* host arena B, uncompressed (target of MOVE) */
    JS_PLACE_COMPRESSED,    /* delta against parent realization, zero-run coded, in memory */
    JS_PLACE_SPILLED,       /* uncompressed in the spill file */
    JS_PLACE_EVICTED        /* no bytes anywhere; rebuilt from the recipe */
} JsPlacement;

typedef enum {
    JS_ACT_REUSE = 0,       /* an existing realization of the same semantic unit and type */
    JS_ACT_COPY,            /* private copy before a write */
    JS_ACT_REFERENCE,       /* child branch points at the parent's realization */
    JS_ACT_RECOMPUTE,       /* rebuild bytes from the recipe */
    JS_ACT_MOVE,            /* relocate bytes to another arena */
    JS_ACT_COMPRESS,
    JS_ACT_SPILL,
    JS_ACT_EVICT,
    JS_ACT_COUNT
} JsAction;

typedef enum { JS_RECIPE_ROOT = 1, JS_RECIPE_DERIVE, JS_RECIPE_EDIT } JsRecipeKind;

/* A representation. derive() must be a pure function of (prev, token): the same
 * inputs give the same bytes. prev is NULL for the root, where token is the seed. */
typedef struct {
    JsRealType type;
    const char *name;
    size_t unit_bytes;
    void (*derive)(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n);
} JsRealizer;

typedef struct JsReal JsReal;

/* One realization of one semantic state unit. */
struct JsReal {
    JsSemId semantic_state_id;
    JsRealType realization_type;
    const JsRealizer *realizer;
    JsReal *parent_realization;     /* recipe input; held by reference */
    JsRecipeKind recipe;
    uint64_t token;                 /* DERIVE input or ROOT seed */
    uint32_t edit_off, edit_len;    /* EDIT patch */
    uint8_t *edit_bytes;
    uint32_t refs;                  /* branches + child recipes + index entries holding it */
    uint32_t holders;               /* branches holding it as a state unit */
    JsPlacement placement;
    uint8_t *bytes;                 /* HOT/COLD: unit_bytes; else NULL */
    uint8_t *packed;                /* COMPRESSED: coded bytes */
    size_t packed_len;
    uint64_t spill_off;             /* SPILLED: offset in the spill file */
    uint8_t content[32];            /* SHA-256 of the realized bytes, fixed at creation */
    uint64_t last_use;              /* logical clock of the last read */
    uint64_t reads;
    JsReal *index_next;             /* semantic reuse index chain */
};

/* Measured costs, all in nanoseconds or bytes. The policy reads nothing else. */
typedef struct {
    double derive_ns;               /* one derive of one unit, per realizer (set per call) */
    double copy_ns_per_byte;
    double compress_ns_per_byte;
    double decompress_ns_per_byte;
    double spill_write_ns_per_byte;
    double spill_read_ns_per_byte;
    double move_ns_per_byte;
    double retain_ns_per_byte;      /* price of keeping one byte resident under pressure */
    double compress_ratio;          /* measured packed/unpacked for this realizer's deltas */
} JsCosts;

/* A branch as J-Space sees it. The state arrays are the physical view of the
 * semantic sequence; SharedStateRealization below is the accounting view. */
typedef struct {
    uint32_t branch_id;
    uint32_t parent_branch;         /* UINT32_MAX for a root */
    uint32_t common_ancestor;       /* root of this lineage */
    uint32_t divergence_point;      /* number of units inherited at fork */
    bool frozen;                    /* has children */
    uint32_t n_units, cap_units;
    JsReal **units;                 /* units[i] realizes semantic unit i */
    const JsRealizer *realizer;
} JsBranch;

/* The SharedStateRealization record of one branch, derived on demand. */
typedef struct {
    JsSemId semantic_state_id;      /* the branch's semantic identity */
    JsRealType realization_type;
    uint32_t parent_realization;    /* parent branch id, UINT32_MAX for a root */
    uint32_t n_shared;              /* units whose realization another holder also holds */
    uint32_t n_private;
    uint64_t resident_bytes;        /* bytes of this branch's units held in memory */
    uint32_t placement_count[6];    /* index by JsPlacement */
    double recompute_cost_ns, transfer_cost_ns, retain_cost_ns, eviction_cost_ns;
} JsSharedStateRealization;

typedef struct {
    uint64_t actions[JS_ACT_COUNT];
    uint64_t derives;               /* realizer derive() calls */
    uint64_t bytes_copied;
    uint64_t live_reals;
    uint64_t resident_bytes;        /* HOT + COLD + COMPRESSED bytes */
    uint64_t peak_resident_bytes;
    uint64_t spilled_bytes;
    uint64_t corrupt_restores;
} JsStats;

#define JS_MAX_BRANCHES 4096u
#define JS_INDEX_BUCKETS 65536u

typedef struct {
    JsBranch *branches[JS_MAX_BRANCHES];
    uint32_t n_branches;
    JsReal *index[JS_INDEX_BUCKETS];
    JsCosts costs;
    JsStats stats;
    uint64_t clock;
    int spill_fd;
    uint64_t spill_end;
    bool reuse_enabled;
    /* Bytes of the last realization rebuilt, so a sequential read of a
     * rebuilt chain does not replay the chain once per unit. Realized bytes
     * never change, so the entry stays valid while that realization lives. */
    const JsReal *cache_r;
    uint8_t *cache_bytes;
    size_t cache_len;
} JsSpace;

int  js_space_init(JsSpace *s, const char *spill_path);
void js_space_destroy(JsSpace *s);

/* Measure the costs the policy reads, for this realizer on this machine. */
void js_calibrate(JsSpace *s, const JsRealizer *r);

/* Semantic identities. */
JsSemId js_sem_root(uint64_t seed);
JsSemId js_sem_derive(const JsSemId *prev, uint64_t token);
JsSemId js_sem_edit(const JsSemId *prev, uint32_t off, const uint8_t *patch, uint32_t len);
/* Identity of a whole branch: SHA-256 over its ordered unit identities. */
JsSemId js_branch_semantic_id(const JsBranch *b);

/* Branch operations. */
int  js_branch_root(JsSpace *s, const JsRealizer *r, uint64_t seed, uint32_t *out);
int  js_branch_fork(JsSpace *s, uint32_t parent, uint32_t *out);
/* Append one unit derived from the last one. */
int  js_branch_derive(JsSpace *s, uint32_t b, uint64_t token);
/* Overwrite len bytes of unit idx. Never touches a realization another holder has. */
int  js_branch_edit(JsSpace *s, uint32_t b, uint32_t idx, uint32_t off,
                    const uint8_t *patch, uint32_t len);
/* Read unit idx into out (unit_bytes). Restores the bytes if they are not resident. */
int  js_branch_read(JsSpace *s, uint32_t b, uint32_t idx, uint8_t *out);
/* SHA-256 over every unit's realized bytes, in order. Restores as needed. */
int  js_branch_content_digest(JsSpace *s, uint32_t b, uint8_t out[32]);
int  js_branch_release(JsSpace *s, uint32_t b);
void js_branch_realization(JsSpace *s, uint32_t b, JsSharedStateRealization *out);

/* Direct physical operations (the policy calls these; tests may too). None of
 * them changes a semantic identity or the bytes a later read returns. */
int  js_real_move(JsSpace *s, JsReal *r);
int  js_real_compress(JsSpace *s, JsReal *r);
int  js_real_spill(JsSpace *s, JsReal *r);
int  js_real_evict(JsSpace *s, JsReal *r);
int  js_real_restore(JsSpace *s, JsReal *r);   /* back to HOT; RECOMPUTE when evicted */

/* FORGE branch policy. Choose the cheapest way to bring resident bytes under
 * budget: for each candidate it compares expected restore cost against the
 * retain price, using only JsCosts and the realization's own history. */
typedef struct {
    uint64_t considered;
    uint64_t chosen[JS_ACT_COUNT];
    uint64_t kept;                  /* candidates for which keeping was cheapest */
} JsPolicyReport;
JsAction js_forge_choose(const JsSpace *s, const JsReal *r, double pressure);
int  js_forge_enforce(JsSpace *s, uint64_t budget_bytes, JsPolicyReport *rep);

const char *js_action_name(JsAction a);
const char *js_placement_name(JsPlacement p);
const char *js_real_type_name(JsRealType t);

#endif /* RX_JSPACE_H */
