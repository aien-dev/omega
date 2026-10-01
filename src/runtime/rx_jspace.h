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
#define JS_ERR_STALE     -7   /* handle generation is not current: the object was reclaimed */
#define JS_ERR_REMOTE    -8   /* not locally owned; this build has no remote transport */
#define JS_ERR_OWNER     -9   /* caller is not the recorded owner */

#include <pthread.h>

typedef struct { uint8_t b[32]; } JsSemId;

/* ---- production mechanics (M20) -------------------------------------------
 *
 * Logical identity is separate from physical address. A realization has a
 * stable JsRealId {slot, gen}; a branch has a stable JsBranchRef {id, gen}.
 * Both survive js_space_commit / js_space_open unchanged. A slot is reused
 * only after its generation advances, so a reference to a reclaimed object
 * is refused with JS_ERR_STALE and never aliases the new occupant. The
 * legacy uint32_t branch-id API stays: the id is the slot, and it is only
 * safe while the caller still holds the branch. Code that keeps a branch
 * beyond one call (World fields, other subsystems) keeps a JsBranchRef.
 *
 * Ownership rules (J-Space storage):
 *  - J-Space holds runtime state-space material only. Memory, evidence and
 *    provenance belong to Cortex; J-Space never records them.
 *  - A realization lives while a branch unit or a child recipe holds it
 *    (refs). Reclamation is immediate in memory; spill/patch extents that a
 *    durable checkpoint may still name are quarantined until the next
 *    js_space_commit makes a checkpoint that no longer names them.
 *  - A branch has one owner subject (0 = the space itself). Owner-checked
 *    release (js_branch_release_ref) refuses other subjects.
 *  - Shared logical state is published only through a World commit. A writer
 *    forks a STAGED branch, works on it privately, then publishes its packed
 *    JsBranchRef as a World field value through the World's own commit path.
 *    After the commit the branch is sealed (js_branch_seal); staged branches
 *    a World commit never named are reclaimed (js_space_reclaim_staged) and
 *    are never persisted. A branch a World field names is never mutated in
 *    place: once forked it is frozen, so older World generations keep reading
 *    the bytes they named.
 *
 * Concurrency: one space mutex (recursive) serializes every public call,
 * matching the World reference's single world mutex. Realized bytes never
 * change after publication, so a read returns the bytes of the generation
 * the caller named. Raw JsReal pointers are only valid while a holder keeps
 * the realization alive.
 *
 * Machine placement: JsHome is a placeholder for the canonical machine
 * identity being defined by M20 Agent 1. `machine` is an opaque fixed-size
 * byte field; J-Space never interprets it beyond equality. Fabric is not
 * implemented: mutating a branch whose home is not JS_HOME_LOCAL, and
 * reading non-resident units of a REMOTE_OWNED branch, fail JS_ERR_REMOTE.
 */
#define JS_MACHINE_ID_BYTES 32u   /* == AIEN_MID_ID_BYTES; holds AienMachineId bytes (placement is still a placeholder) */

typedef enum {
    JS_HOME_LOCAL = 0,          /* owned and authoritative on this machine */
    JS_HOME_REMOTE_OWNED = 1,   /* authoritative elsewhere; no local writes */
    JS_HOME_REPLICA = 2         /* local read-only copy of a remote owner's branch */
} JsLocality;

typedef struct {
    uint8_t machine[JS_MACHINE_ID_BYTES];   /* opaque canonical machine identity bytes */
    uint32_t locality;                      /* JsLocality */
} JsHome;

typedef struct { uint32_t slot; uint32_t gen; } JsRealId;
typedef struct { uint32_t id; uint32_t gen; } JsBranchRef;

/* A branch reference fits one 64-bit World field. */
static inline uint64_t js_branch_ref_pack(JsBranchRef r) {
    return ((uint64_t)r.gen << 32) | r.id;
}
static inline JsBranchRef js_branch_ref_unpack(uint64_t v) {
    JsBranchRef r = { (uint32_t)v, (uint32_t)(v >> 32) };
    return r;
}

/* Hard limits. Exceeding one fails deterministically with JS_ERR_FULL before
 * anything changes. Zero for a byte limit means unlimited. */
typedef struct {
    uint32_t max_reals;             /* live realizations (slab slots) */
    uint32_t max_branches;          /* branch slots, at most JS_MAX_BRANCHES */
    uint64_t max_spill_bytes;       /* extent space in the spill/data file */
    uint64_t max_resident_bytes;    /* HOT + COLD + COMPRESSED bytes */
} JsLimits;

typedef struct { uint64_t off, len; } JsExtent;

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
    /* M20: stable identity and durable recipe input. */
    uint32_t slot, gen;             /* JsRealId; the slab never moves a JsReal */
    uint64_t patch_off;             /* EDIT patch extent in the data file, UINT64_MAX if none */
    JsReal *free_next;              /* slab free list */
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
    /* M20 */
    uint32_t gen;                   /* JsBranchRef generation of this occupant */
    uint32_t owner;                 /* owner subject, 0 = the space */
    bool staged;                    /* not yet named by a World commit; never persisted */
    JsHome home;
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
    /* ---- M20 production mechanics ---- */
    JsLimits limits;
    pthread_mutex_t mu;             /* recursive; serializes every public call */
    bool mu_ready;
    uint32_t branch_gen[JS_MAX_BRANCHES];
    uint32_t free_branch[JS_MAX_BRANCHES];
    uint32_t n_free_branch;
    JsReal **slab;                  /* chunks of JS_SLAB_CHUNK realizations */
    uint32_t slab_chunks;
    uint32_t real_hw;               /* slots ever handed out */
    uint32_t real_gen_floor;        /* first generation of a never-used slot */
    JsReal *real_free;
    JsExtent *ext_free;             /* free extents, sorted by offset, coalesced */
    uint32_t n_ext_free, cap_ext_free;
    JsExtent *ext_pending;          /* freed but maybe named by the last checkpoint */
    uint32_t n_ext_pending, cap_ext_pending;
    bool durable;
    char *dir;
    uint64_t commit_seq;
    JsHome local_home;              /* home given to locally created branches */
} JsSpace;

#define JS_SLAB_CHUNK 1024u

/* Anonymous, volatile space: the spill file is unlinked at once. Default limits. */
int  js_space_init(JsSpace *s, const char *spill_path);
/* Same, with explicit limits (NULL = defaults). */
int  js_space_init_limits(JsSpace *s, const char *spill_path, const JsLimits *lim);
void js_space_destroy(JsSpace *s);
void js_limits_default(JsLimits *lim);

/* Durable space in directory `dir` (created if missing): data file
 * jspace.data (spill + EDIT patches, fixed extents) and metadata checkpoint
 * jspace.meta. If a checkpoint exists, it is validated and loaded: every
 * branch, realization id and generation comes back; realizations reopen as
 * SPILLED (if their extent was checkpointed) or EVICTED and rebuild from their
 * recipe. `realizers` must name every realization type the checkpoint uses
 * (matched by type and unit_bytes). A torn or corrupt checkpoint fails with
 * JS_ERR_CORRUPT and leaves no state; a leftover temporary file is ignored. */
int  js_space_open(JsSpace *s, const char *dir, const JsRealizer *const *realizers,
                   uint32_t n_realizers, const JsLimits *lim, const JsHome *local_home);
/* Make the current non-staged state the durable checkpoint: data first
 * (fdatasync), then metadata to a temporary file, fsync, atomic rename,
 * directory fsync. Only then are quarantined extents reusable. */
int  js_space_commit(JsSpace *s);

/* Stable references. */
int  js_branch_ref(JsSpace *s, uint32_t id, JsBranchRef *out);
int  js_branch_check(JsSpace *s, JsBranchRef ref);          /* JS_OK or JS_ERR_STALE */
int  js_real_id(JsSpace *s, uint32_t b, uint32_t idx, JsRealId *out);
/* NULL when the slot was reclaimed (generation mismatch). Valid while held. */
JsReal *js_real_lookup(JsSpace *s, JsRealId id);

/* Ownership and staging. */
int  js_branch_fork_staged(JsSpace *s, JsBranchRef parent, uint32_t owner, JsBranchRef *out);
int  js_branch_seal(JsSpace *s, JsBranchRef b);              /* a World commit named it */
int  js_branch_release_ref(JsSpace *s, JsBranchRef b, uint32_t subject);
/* Release every branch still staged. Call when no commit can name them. */
uint32_t js_space_reclaim_staged(JsSpace *s);
int  js_branch_set_owner(JsSpace *s, JsBranchRef b, uint32_t owner);
int  js_branch_set_home(JsSpace *s, JsBranchRef b, const JsHome *home);
int  js_branch_home(JsSpace *s, JsBranchRef b, JsHome *out);
/* COMPOSITION-2: lifecycle facts of one branch, read under the space lock
 * (JS_ERR_STALE for a reclaimed reference). parent_gen is the generation of
 * the parent slot's current occupant, 0 when the branch is a root or the
 * parent slot is empty. */
typedef struct {
    uint32_t owner;
    uint32_t parent, parent_gen;
    bool staged;
    uint32_t locality;              /* JsLocality */
} JsBranchInfo;
int  js_branch_info(JsSpace *s, JsBranchRef b, JsBranchInfo *out);

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
