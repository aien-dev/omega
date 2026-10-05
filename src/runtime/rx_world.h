/*
 * rx_world.h -- first heartbeat of the resident reaction runtime (ADR 0016).
 *
 * One shared object world. Reactions declare what wakes them, what they read,
 * what they write and which capabilities they need. A dependency index maps a
 * (object, field-mask) change to the interested reactions only; nothing scans
 * the graph. Worker threads run whatever is READY, in admission order; they
 * never decide the semantic sequence. A reaction computes against a versioned
 * snapshot, then publishes atomically after re-validating its inputs and its
 * capabilities. Every outcome leaves a content-addressed causal crumb.
 *
 * This is the simple human reference (ADR 0016 §56 step 2): one world mutex,
 * bounded fixed-size tables. It is deliberately not optimized.
 *
 * Covered here: one canonical object (semantic record and physical placement
 * are two aspects of that same object), CPU checks of the pointer-free
 * cross-engine descriptor, R3 reaction core, R4 causal crumbs, R5 host
 * admission, R6 host stability. The receipt is what claims a gate. This file
 * does not claim a graphics-processor run. A native authority view can be
 * installed beside the Linux oracle; installing it is not, by itself, the
 * R7 claim. Faculty labels in tests are not the real AIEN, Omega, or AEGIS.
 */
#ifndef RX_WORLD_H
#define RX_WORLD_H

#include "rx_caproot.h"
#include "rx_caller.h"
#include "omega_shared_world_abi.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define RX_MAX_OBJECTS      256u
#define RX_MAX_FIELDS       8u
#define RX_MAX_REACTIONS    1024u
#define RX_MAX_DEPS         8u
#define RX_MAX_WRITES       8u
#define RX_MAX_CAPS         8u
#define RX_MAX_MUTATIONS    16u
#define RX_MAX_SUBS         1024u  /* subscriptions per object */
/* Causal-log capacity for long living-body episodes: 2^25 records (about
 * 38 GB of reserved address space at 1128 B each), backed only as written. */
#define RX_CRUMBS_LONG_EPISODE (1ull << 25)
#define RX_MAX_WORKERS      16u
#define RX_MAX_BINDERS      4u     /* commit binders per World (COMPOSITION-2) */
#define RX_MAX_PARENTS      (RX_MAX_DEPS * RX_MAX_FIELDS + 1u)
#define RX_PRIORITY_CLASSES 7u
#define RX_DEFERRED_INITIAL 64u

#define RX_ALL_FIELDS       ((uint64_t)((1u << RX_MAX_FIELDS) - 1u))
#define RX_FIELD(i)         ((uint64_t)1u << (i))

/* Faculties (logical write domains, not memory owners). */
enum { RX_FACULTY_EXTERNAL = 0, RX_FACULTY_AIEN, RX_FACULTY_OMEGA, RX_FACULTY_AEGIS,
       RX_FACULTY_ROOT };   /* the capability root installing what it minted (R8) */

/* Priority classes (ADR 0016 §9), highest first. */
enum {
    RX_PRIO_CRITICAL = 0, RX_PRIO_INTERACTIVE, RX_PRIO_FOREGROUND, RX_PRIO_LEARNING,
    RX_PRIO_MAINTENANCE, RX_PRIO_SPECULATIVE, RX_PRIO_BACKGROUND
};

/* How a self-fed cycle should be read. Ordinary alternating output is an
 * unstable oscillation. A declared periodic loop may alternate and still be
 * useful control. A run that stops changing is convergent. A run that keeps
 * spending work without a new value is livelock. */
enum { RX_LOOP_ORDINARY = 0, RX_LOOP_PERIODIC = 1 };

/* Persistence class (ADR 0016 §19; ADR 0015 §2 mapping in ADR 0016 Part I). */
typedef enum { RX_PERSIST_EPHEMERAL = 0, RX_PERSIST_RESIDENT, RX_PERSIST_DURABLE } RxPersist;

/* Reaction lifecycle (ADR 0016 §6). Every activation ends back in DORMANT. */
typedef enum {
    RX_DORMANT = 0,
    RX_READY,
    RX_BLOCKED_AUTHORITY,
    RX_BLOCKED_RESOURCE,
    RX_RUNNING,
    RX_PUBLISHING,
    RX_COMMITTED,
    RX_INVALIDATED,
    RX_FAILED,
    RX_CANCELLED,
    RX_CONFLICT,
    RX_REJECTED,
    RX_STATE_COUNT
} RxState;

/* Causal crumb kinds. */
typedef enum {
    RX_CRUMB_CREATE = 1,
    RX_CRUMB_EXTERNAL,
    RX_CRUMB_COMMIT,
    RX_CRUMB_INVALIDATED,
    RX_CRUMB_BLOCKED_AUTHORITY,
    RX_CRUMB_REJECTED,
    RX_CRUMB_FAILED,
    RX_CRUMB_NOOP,             /* ran, proposed no change: fixed point */
    RX_CRUMB_RETIRE,
    RX_CRUMB_QUARANTINE,       /* an R6 limit stopped this reaction; reason names it */
    RX_CRUMB_CANCELLED,        /* HD-09: ran past its declared deadline; nothing
                                  published; reason RX_ERR_DEADLINE, or
                                  RX_ERR_HALTED when an operator stop refused it */
    /* R16 G6 operator emergency control (spec/r16-operator-emergency-stop.md):
     * reaction UINT32_MAX, caps[0] the operator's control capability. */
    RX_CRUMB_OPERATOR_STOP,    /* a stop took effect; reason = the operator's code */
    RX_CRUMB_OPERATOR_RESUME   /* a stopped world resumed; parent = its stop */
} RxCrumbKind;

/* Which R6 limit engaged (reason of a QUARANTINE crumb). */
enum { RX_CONTAIN_BUDGET = 1, RX_CONTAIN_OSCILLATION, RX_CONTAIN_LIVELOCK,
       RX_CONTAIN_CONFLICT };

/* Publication errors. */
#define RX_OK               0
#define RX_ERR_ARG         -1
#define RX_ERR_FULL        -2
#define RX_ERR_STALE_GEN   -3
#define RX_ERR_AUTHORITY   -4
#define RX_ERR_WRITE_SET   -5
#define RX_ERR_NOT_FOUND   -6
#define RX_ERR_TIMEOUT     -7
#define RX_ERR_BOUNDS      -20
#define RX_ERR_REPLAY      -21
#define RX_ERR_TORN        -22
#define RX_ERR_BAD_DESC    -23
#define RX_ERR_UNPLACED    -24
#define RX_ERR_EXISTS      -25
#define RX_ERR_SEAT_LOST   -26   /* the graphics seat died holding the claim */
#define RX_ERR_IDENTITY    -27   /* caller credential absent, forged, stale or revoked */
#define RX_ERR_BINDING     -28   /* a bound field's external reference was refused (COMPOSITION-2) */
#define RX_ERR_BUSY        -29   /* the reaction still has work pending (rx_world_remove_reaction) */
#define RX_ERR_DEADLINE    -30   /* the logical tick passed the declared deadline (RX_CRUMB_CANCELLED) */
#define RX_ERR_HALTED      -31   /* operator emergency stop in force (RX_CRUMB_CANCELLED) */
#define RX_ERR_IO          -32   /* an operator-stop mark could not be read or retired */

/* Reaction notices carried in the frozen 128-byte descriptor.
 * The older transform request/result values stay in the frozen layout and
 * are rejected here: a ring slot is a wake, a claim, a publication, a
 * completion, a fault, or a shutdown. It is not a call to another subsystem.
 */
enum {
    RX_RING_WAKE       = 0x0010u,
    RX_RING_CLAIM      = 0x0011u,
    RX_RING_PUBLISH    = 0x0012u,
    RX_RING_COMPLETE   = 0x0013u,
    RX_RING_FAULT      = 0x0014u,
    RX_RING_KEEPALIVE  = 0x0003u, /* frozen NOP: ordering only, not a call */
    RX_RING_SHUTDOWN   = 0x0015u
};

/* Host-side descriptor faults. They do not change the frozen mailbox enum. */
#define RX_FAULT_BOUNDS    0x0101u
#define RX_FAULT_CAP       0x0102u
#define RX_FAULT_TORN_PUB  0x0103u
#define RX_FAULT_OVERLAP   0x0104u
#define RX_FAULT_DIVERGED  0x0105u
#define RX_FAULT_UNPLACED  0x0106u

/* Capability references in publication and claim payloads. Bytes 0-3 are
 * the input object's cap id and 4-7 the low half of its 64-bit generation;
 * a claim also carries the output object's id (24), object generation (28),
 * cap id (32) and cap generation low half (36). The high halves of the two
 * cap generations sit at 40 and 44, past every field the seat reads; the
 * seat copies the whole descriptor into its notice, so they come back. */
#define RX_CAP_GEN_HI_A    40u
#define RX_CAP_GEN_HI_B    44u
#define RX_CAP_PAYLOAD     48u

/* Physical aspect. These name where the bytes sit. They are not the object's name.
 * A live object may have no placement. One spare window exists so a placed
 * object can move without borrowing another object's bytes. */
#define RX_PLACE_COHERENT    1u
#define RX_LOCALITY_MACHINE  1u
#define RX_COHERENCY_HOST    1u   /* CPU image of the shared layout; no graphics processor claimed */
#define RX_COHERENCY_SEAT    2u   /* same window, graphics seat participated */
/* Bit in RxResourceNeed.accelerator_features. Admission already requires the
 * budget to offer every bit a reaction asks for. This bit selects the resident
 * graphics seat. It is not a call from the processor to a graphics function. */
#define RX_ACCEL_BLACKWELL   0x2u
#define RX_SEAT_BLACKWELL    0x100u
#define RX_SEQ_WORKER        0x200u   /* crumb worker of the R15 sequential reference */
#define RX_OBJECT_WINDOW     64u  /* eight field values, little-endian */
#define RX_PHYS_WINDOWS      (RX_MAX_OBJECTS + 1u)

typedef struct { uint32_t id; uint32_t generation; } RxObjRef;

typedef struct {
    RxObjRef obj;
    uint64_t mask;              /* fields of interest */
} RxDep;

typedef struct {
    RxCapRef ref;
    uint64_t resource;
    uint32_t rights;
} RxCapNeed;

typedef struct {
    RxObjRef obj;
    uint32_t field;
    uint64_t value;
} RxMutation;

/* Versioned snapshot of one read dependency. */
typedef struct {
    RxObjRef obj;
    uint64_t mask;
    uint64_t version;
    uint64_t field[RX_MAX_FIELDS];
    uint64_t field_version[RX_MAX_FIELDS];
    uint64_t field_writer[RX_MAX_FIELDS];   /* causal id of the last writer */
} RxSnapshotDep;

typedef struct RxCtx {
    uint32_t n_in;
    RxSnapshotDep in[RX_MAX_DEPS];
    uint32_t n_out;
    RxMutation out[RX_MAX_MUTATIONS];
    void *user;
    uint32_t worker;
} RxCtx;

/* A reaction function may return RX_FN_DEFER when it has handed a physical
 * effect it decided (an R9 store write) to a durable executor and has
 * nothing to publish yet. The activation keeps its admission and its place
 * (READY/RUNNING is not left, quiescence waits for it) but frees the worker.
 * When the executor calls rx_world_resume the same activation runs again on a
 * fresh snapshot, on an ordinary worker, and publishes through the usual
 * checks. No crumb is written for the deferring run. Never returned in a
 * sequential reference world (it is treated as a failure there). */
#define RX_FN_DEFER 0x44454652

/* Pure transformation: read ctx->in, append proposals to ctx->out.
 * Return 0 to publish (possibly nothing), negative to fail. */
typedef int (*RxFn)(RxCtx *ctx);

/* What a reaction costs to be admitted. Zero means "none of this kind".
 * Priority stays on the descriptor; this does not choose meaning, only fit. */
typedef struct {
    uint32_t compute_class;         /* 0 = any; else bit (class & 31) must be offered */
    uint32_t locality;              /* 0 = anywhere; else required bits */
    uint32_t accelerator_features;  /* 0 = none; else required bits */
    uint32_t latency_class;         /* recorded; admission uses priority */
    uint64_t memory_bytes;
    uint64_t deadline;              /* 0 = none, compared with the world tick */
    uint64_t energy_cost;           /* 0 = free */
} RxResourceNeed;

/* Physical budget. The setter is exact: zero means that resource is
 * unavailable. rx_world_init installs permissive defaults for callers that
 * never set a budget. starvation_bound 0 disables bounded fairness (strict
 * priority); a positive value bounds how long a waiting lower class may be
 * skipped by more urgent work. */
typedef struct {
    uint32_t slots;
    uint32_t starvation_bound;
    uint32_t offered_locality;
    uint32_t offered_accel;
    uint32_t compute_mask;
    uint64_t memory_bytes;
    uint64_t energy_budget;
    uint64_t logical_tick;
} RxResourceBudget;

/* Stability limits. Zero disables that limit. */
typedef struct {
    uint32_t activation_budget;     /* per reaction */
    uint32_t fanout_limit;          /* wakes from one publication */
    uint32_t oscillation_limit;     /* A-B-A flips before quarantine */
    uint32_t livelock_limit;        /* consecutive no-progress runs */
    uint32_t conflict_limit;        /* consecutive invalidations/rejections */
} RxStabilityBudget;

typedef struct {
    const char *name;
    uint32_t faculty;
    uint32_t subject;           /* principal the capabilities must be bound to */
    /* R16 C5: in a world with bound callers, the runtime-issued credential
     * for `subject` (rx_caller.h). Checked at registration; the stored copy
     * keeps only the generation, and activation and commit re-check that the
     * enrollment is still live at it. Ignored by an unbound world. */
    RxCallerCred caller;
    uint32_t priority;
    RxResourceNeed need;
    uint32_t n_triggers;
    RxDep triggers[RX_MAX_DEPS];
    uint32_t n_reads;           /* triggers are read implicitly */
    RxDep reads[RX_MAX_DEPS];
    uint32_t n_writes;
    RxDep writes[RX_MAX_WRITES];
    uint32_t n_caps;
    RxCapNeed caps[RX_MAX_CAPS];
    /* R8. When cap_slotted[i] is set, the reference for caps[i] is read at
     * every check from fields 0 (cap id) and 1 (generation) of the slot
     * object, not from caps[i].ref. Holding a slot is still not permission:
     * the authority validates whatever reference it holds. A slot that is
     * retired or of another generation yields no reference. */
    bool cap_slotted[RX_MAX_CAPS];
    RxObjRef cap_slot[RX_MAX_CAPS];
    /* R8. Every field this reaction proposes is stamped as written by its
     * commit, even when the value did not change (only changed fields wake
     * anyone). For records whose authorship is checked field by field: a
     * value left over from an earlier writer would otherwise keep that
     * writer's name. */
    bool stamp_proposed;
    uint32_t loop_kind;        /* RX_LOOP_ORDINARY or RX_LOOP_PERIODIC */
    RxFn fn;
    void *user;
} RxReactionDesc;

typedef struct {
    uint32_t id;
    uint32_t generation;
    uint32_t type;
    bool live;
    RxPersist persist;
    uint64_t resource;          /* capability resource that guards this object */
    uint64_t version;
    uint64_t field[RX_MAX_FIELDS];
    uint64_t field_version[RX_MAX_FIELDS];
    uint64_t field_writer[RX_MAX_FIELDS];
    uint8_t digest[32];         /* content identity of (type, fields) */
    /* Capability reference. Holding this pair is not permission; the root still checks it. */
    RxCapRef cap;
    /* Physical aspect of this same object. Absent until attached.
     * Not a second object and not a second generation. */
    bool placed;
    uint32_t window;
    uint64_t region_offset;
    uint64_t size_bytes;
    uint32_t placement;
    uint32_t locality;
    uint32_t coherency;
} RxObject;

typedef struct { uint32_t reaction; uint32_t generation; uint64_t mask; } RxSub;

typedef struct {
    uint64_t id;                /* 1-based, dense; 0 = none */
    RxCrumbKind kind;
    uint32_t reaction;          /* UINT32_MAX for external/create */
    uint32_t faculty;
    uint32_t worker;
    uint64_t wake_cause;        /* crumb that last woke this activation */
    uint64_t coalesced_wakes;   /* extra wakes folded into this activation */
    uint32_t n_inputs;
    struct { RxObjRef obj; uint64_t version; uint64_t mask; } inputs[RX_MAX_DEPS];
    uint32_t n_caps;
    RxCapRef caps[RX_MAX_CAPS];
    uint32_t cap_issuer[RX_MAX_CAPS]; /* authority provenance; 0 if unknown */
    uint32_t n_outputs;
    struct { RxObjRef obj; uint64_t version; uint64_t mask; } outputs[RX_MAX_WRITES];
    uint32_t n_parents;
    uint64_t parents[RX_MAX_PARENTS];
    uint64_t t_start_ns;
    uint64_t t_end_ns;
    int32_t reason;
    uint8_t digest[32];         /* SHA-256 over canonical crumb bytes + parent digests */
    /* Root of the causal episode: the outside publication this crumb's wake
     * chain starts from (its own id for one), 0 when the chain starts at no
     * outside publication. Derived from wake_cause, so not hashed; checked by
     * rx_world_verify_crumbs. */
    uint64_t episode;
} RxCrumb;

typedef struct {
    RxReactionDesc desc;
    RxState state;
    bool rearm;                 /* woken while running: go READY again after */
    bool quarantined;
    bool parked;                /* yielded so other admitted work can run */
    bool holding;               /* currently charged against the physical budget */
    uint64_t wake_cause;
    uint64_t coalesced;
    uint64_t activations;          /* lifetime observability */
    uint64_t episode_activations;  /* reset when a wake belongs to another causal episode */
    uint64_t episode;              /* episode root episode_activations counts for */
    uint64_t last_crumb;           /* this reaction's latest crumb */
    uint32_t contain_pending;      /* RX_CONTAIN_* engaged in this activation, not yet recorded */
    uint64_t commits;
    uint64_t suppressed;
    uint64_t wait_seq;
    uint64_t service;
    uint64_t memory_held;
    uint64_t energy_held;
    uint32_t noop_streak;
    uint32_t osc_streak;
    uint32_t conflict_streak;
    uint32_t yield_left;        /* self-wakes to skip while other work is waiting */
    uint64_t last_out;
    uint64_t prev_out;
    bool have_last;
    bool have_two;
    bool resident_seat;         /* handed to the graphics seat; fn is not called */
    bool deferred;              /* fn returned RX_FN_DEFER; waits for rx_world_resume */
    bool resume_pending;        /* resumed before the deferring run returned */
    /* I11: popped while a running writer of one of its inputs had not yet
     * published. Admitted (charged, in in_flight), state READY, not on a
     * ready ring; released when no such writer is running, so the writer's
     * wake merges into this activation instead of causing a second one. */
    bool upstream_held;
    /* Taken out by rx_world_remove_reaction: no subscriptions, never woken
     * or run; the slot may be reused by a registration with the same subject
     * and faculty. */
    bool removed;
    /* R15 timing (only with a timing buffer). */
    uint64_t t_demand, t_ready, t_run, t_fn_end, sched_ns, sched_cpu_ns;
    /* Sequential reference only (rx_seq_reference.c): the newest version of
     * each trigger this stage has seen, and a re-run it still owes. */
    uint64_t seq_seen[RX_MAX_DEPS];
    bool seq_pending;
    uint64_t resident_seq;      /* ring sequence of the claim notice */
    uint64_t resident_parent;   /* crumb that made this activation ready */
} RxReaction;

typedef struct {
    uint64_t transitions[RX_STATE_COUNT][RX_STATE_COUNT];
    uint64_t illegal_transitions;
    uint64_t wakes;
    uint64_t coalesced_wakes;
    uint64_t subscriptions_checked;
    uint64_t commits;
    uint64_t invalidations;
    uint64_t blocked_authority;
    uint64_t blocked_resource;
    uint64_t rejected;
    uint64_t failed;
    uint64_t noops;
    uint64_t crumb_overflow;
    uint64_t suppressed_wakes;
    uint64_t deferred_wakes;
    uint64_t deferred_peak;
    uint64_t quarantines;
    uint64_t oscillation_trips;
    uint64_t livelock_trips;
    uint64_t backoffs;
    uint64_t periodic_commits;
    uint64_t useful_commits;
    uint64_t churn;
    uint64_t deadline_overdue;
    uint64_t desc_published;
    uint64_t desc_rejected;
    uint64_t resident_claims;   /* claims posted to a resident seat */
    uint64_t resident_closed;   /* claims ended: committed, refused, or lost */
    uint64_t deferrals;         /* activations that handed an effect to a durable executor */
    uint64_t seat_losses;       /* times a seat was declared lost */
    /* R15 instrumentation. Counted, never inferred. */
    uint64_t activations;       /* run_one entries (every activation, any outcome) */
    uint64_t externals;         /* outside publications */
    /* Propagation (spec §6.3). `subscriptions_checked` counts subscriptions
     * inspected and `wakes` counts wake attempts (demand calls, including
     * the sequential reference's own admission). */
    uint64_t subscriptions_matched; /* inspected, generation and mask matched */
    uint64_t wakes_accepted;    /* wake attempt took a DORMANT reaction to waiting */
    uint64_t ready_inserts;     /* reactions placed on a ready ring */
    /* Copied bytes: bytes processor code writes into a destination as a copy
     * or serialization of data that exists elsewhere. Every path is its own
     * counter; none is inferred from architecture. The graphics chip's own
     * ring and window accesses are not processor copies and are not here. */
    uint64_t snapshot_bytes;    /* RxSnapshotDep entries filled into RxCtx */
    uint64_t stage_bytes;       /* current fields copied into the publication
                                   staging buffer, per object written */
    uint64_t crumb_bytes;       /* RxCrumb records appended to the causal log */
    uint64_t proj_bytes;        /* coherent object table entries + field windows */
    uint64_t c2g_write_bytes;   /* descriptors written to the processor->seat ring
                                   (publications, claims, shutdown, injected) */
    uint64_t c2g_read_bytes;    /* descriptors copied out of that ring (publication
                                   consumer, processor stand-in seat) */
    uint64_t g2c_write_bytes;   /* result descriptors written by the stand-in seat */
    uint64_t g2c_read_bytes;    /* result descriptors copied out of the seat ring */
    uint64_t window_move_bytes; /* object windows saved and restored on relocation */
    uint64_t setup_copy_bytes;  /* whole-region copy when binding coherent memory */
    /* Scheduling time, only while a timing buffer is installed: wake,
     * admission and ready-ring code (RES), readiness polling and admission
     * (SEQ). Wall is CLOCK_MONOTONIC; CPU is the calling thread's
     * CLOCK_THREAD_CPUTIME_ID. Each interval is read on one thread. */
    uint64_t sched_wall_ns;
    uint64_t sched_cpu_ns;
    uint64_t gpu_results_taken; /* completion notices taken from the chip ring */
    uint64_t gpu_polls_empty;   /* completion-ring polls that found nothing */
    uint64_t gpu_host_waits;    /* times a caller blocked for a GPU completion */
    uint64_t seq_pulses;        /* sequential reference only: pulses run */
    uint64_t seq_polls;         /* sequential reference only: readiness polls */
    uint64_t seq_runs;          /* sequential reference only: stages run */
    /* HD-09 resource contract v0, first enforcement cut: activations whose
     * function returned after the logical tick passed need.deadline (strictly
     * greater). Each was cancelled before publishing; its charge was refunded
     * once by end_activation. */
    uint64_t deadline_cancelled;
} RxStats;

/* R15 timing sample, recorded only when a timing buffer is installed
 * (rx_world_set_timing). Wall-clock ns, CLOCK_MONOTONIC. */
typedef struct {
    uint32_t reaction;
    uint32_t outcome;           /* RxCrumbKind of the activation */
    uint64_t cause;             /* crumb that woke it */
    uint64_t t_demand;          /* wake accepted (DORMANT -> waiting) */
    uint64_t t_ready;           /* admitted: placed on a ready ring */
    uint64_t t_run;             /* run_one entered */
    uint64_t t_fn_end;          /* reaction function returned */
    uint64_t t_visible;         /* publication committed and dependents woken */
    uint64_t sched_ns;          /* wall time inside wake/admission/queue code for it */
    uint64_t sched_cpu_ns;      /* thread CPU time inside the same code */
} RxTiming;

/* The most recent propagation wave (only while a timing buffer is installed). */
typedef struct {
    uint64_t wall_ns, cpu_ns;
    uint64_t inspected, matched, wake_attempts, wakes_accepted;
    uint64_t ready_inserts, coalesced, deferred, suppressed;
} RxPropWave;

typedef int (*RxAuthValidateFn)(const void *ctx, RxCapRef ref, uint32_t subject,
                                 uint64_t resource, uint32_t rights, RxCapEntry *out);
typedef int (*RxAuthInspectFn)(const void *ctx, RxCapRef ref, RxCapEntry *out);

/* State of the R16 G6 operator emergency stop (rx_world_halt_status). */
typedef struct {
    bool halted;
    bool restored;          /* this stop was read back from the durable mark */
    uint64_t seq;           /* stops taken (a restored stop keeps its number) */
    uint32_t subject;       /* operator of the stop in force or last lifted */
    RxCapRef cap;           /* the control capability that operator presented */
    int32_t reason;         /* the operator's reason code */
    uint64_t crumb;         /* its OPERATOR_STOP crumb (0 if the log was full) */
    uint64_t t_ns;          /* CLOCK_REALTIME of the stop */
    int durable;            /* 1 mark on disk; 0 no halt dir; < 0 -errno of the write */
    uint64_t refused;       /* activations, publications, seat results, creates and retires refused */
} RxHaltStatus;

/* Durable recorder (M20 Cortex). Called with the world mutex held, once per
 * crumb, after the crumb is in the log; it reads the world and must not call
 * back into it. It cannot veto or fail the commit it records. The one
 * installed recorder is rx_cortex_record.c, the Cortex writer for World. */
struct RxWorld;
typedef void (*RxRecordFn)(void *ctx, const struct RxWorld *w, const RxCrumb *k);
typedef void (*RxRecordReleaseFn)(void *ctx);

/* Commit binder (COMPOSITION-2). A bound field holds a reference into a store
 * outside the World (a J-Space branch). The binder is consulted with the world
 * mutex held, after a publication has passed every other check (versions,
 * authority, write set) and before anything in it becomes visible:
 *
 *   check(each changed bound field)   may refuse: nothing is visible, the
 *                                     activation ends REJECTED with
 *                                     RX_ERR_BINDING, abort() runs for every
 *                                     bound value the publication proposed
 *   bind(each changed bound field)    the binder's point of no return (J-Space
 *                                     seals the branch). A failure here also
 *                                     refuses the publication; abort() is told
 *                                     which values were already bound
 *   then the commit is applied        old values stay in the binder's care
 *
 * So a World value never names a reference its store did not accept, and a
 * refused reference never becomes a World value. The binder must not call
 * back into the World. `subject` is the publishing reaction's subject, or the
 * World's external subject. Up to RX_MAX_BINDERS binders per World, each
 * owning the fields it bound (a field has at most one owner); a publication's
 * bound fields are each checked, bound and aborted by their own binder. */
typedef int  (*RxBindCheckFn)(void *ctx, RxObjRef obj, uint32_t field, uint64_t old_value,
                              uint64_t new_value, uint32_t subject);
typedef int  (*RxBindFn)(void *ctx, RxObjRef obj, uint32_t field, uint64_t old_value,
                         uint64_t new_value, uint32_t subject);
typedef void (*RxBindAbortFn)(void *ctx, RxObjRef obj, uint32_t field, uint64_t value,
                              uint32_t subject, int was_bound);

typedef struct RxWorld {
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    pthread_cond_t idle_cv;

    RxCapRoot *root;
    /* When set, validation reads this view instead of the Linux oracle.
     * The view cannot mint. The Linux oracle stays available for comparison. */
    const void *auth_ctx;
    RxAuthValidateFn auth_validate;
    RxAuthInspectFn auth_inspect;
    uint32_t external_subject;

    RxObject objects[RX_MAX_OBJECTS];
    RxSub *subs[RX_MAX_OBJECTS];
    uint32_t n_subs[RX_MAX_OBJECTS];

    RxReaction reactions[RX_MAX_REACTIONS];
    uint32_t n_reactions;

    /* Physical admission: bounded per-priority FIFO rings. */
    uint32_t ready_q[RX_PRIORITY_CLASSES][RX_MAX_REACTIONS];
    uint32_t ready_head[RX_PRIORITY_CLASSES];
    uint32_t ready_len[RX_PRIORITY_CLASSES];
    uint32_t in_flight;         /* READY + RUNNING + PUBLISHING */

    RxResourceBudget budget;
    RxStabilityBudget stability;
    uint32_t used_slots;
    uint64_t used_memory;
    uint64_t used_energy;
    uint32_t admit_debt[RX_PRIORITY_CLASSES];
    uint32_t run_debt[RX_PRIORITY_CLASSES];
    uint64_t admit_seq;
    uint32_t peak_slots;
    uint32_t peak_blocked;

    /* Fan-out limiting delays excess valid wakes; it never drops semantic work. */
    struct { uint32_t reaction; uint64_t cause; } *deferred;
    uint32_t deferred_head;
    uint32_t deferred_len;
    uint32_t deferred_cap;

    RxCrumb *crumbs;
    uint64_t n_crumbs;
    uint64_t crumb_cap;

    pthread_t workers[RX_MAX_WORKERS];
    uint32_t n_workers;
    bool stopping;

    /* CPU image of OMEGA_SHARED_WORLD_V1. The 32-byte records are a projection
     * of objects[], not an allocator of their own. */
    uint8_t *coherent;
    uint64_t coherent_bytes;
    uint32_t world_epoch;
    bool resident_enabled;      /* a resident seat may take claims on this image */
    bool coherent_borrowed;     /* image memory is not freed with the world */
    bool resident_stopped;
    /* Incarnation of the resident seat. Every claim carries it. A seat takes
     * only claims of its own incarnation, and the processor accepts only
     * results of the current one. It moves when a seat is lost; object
     * generations do not. */
    uint32_t seat_generation;

    RxStats stats;
    uint32_t n_deferred;        /* activations waiting for rx_world_resume */
    /* Signalled when a claim is posted to the resident seat, so a completion
     * transport can sleep while no graphics work is outstanding. */
    pthread_cond_t claim_cv;

    /* R15 timing buffer (off when null). */
    RxTiming *timing;
    uint64_t timing_cap, n_timing;   /* n_timing counts attempts, even past cap */
    RxPropWave last_prop;            /* most recent propagate() */
    uint32_t sched_nest;             /* timed scheduling sections open (count once) */

    /* Sequential reference (never production): no workers, no dependency
     * wakes; rx_seq_pulse decides what runs. */
    bool sequential;
    uint64_t seq_pulse_started, seq_idle_start;

    /* R16 C5 caller enrollments (rx_caller.h). Only digests are kept. Own
     * lock: taken inside mu (activation) and alone (R9's check). */
    pthread_mutex_t callers_mu;
    struct {
        uint32_t subject;
        bool live;
        uint64_t generation;
        uint8_t digest[32];
    } callers[RX_CALLER_MAX];
    uint32_t n_callers;
    uint64_t caller_generation;      /* last generation issued */
    bool callers_bound;              /* one way: set once, never cleared */

    /* R16 G6 operator emergency stop (spec/r16-operator-emergency-stop.md).
     * Written under mu and callers_mu together, so either lock reads it.
     * No worker takes work and nothing publishes while it is set. */
    bool halted;
    RxHaltStatus halt;
    char halt_dir[384];              /* durable mark directory; "" = none */

    /* M20 durable recorder (rx_world_set_recorder); at most one. */
    RxRecordFn recorder;
    RxRecordReleaseFn recorder_release;
    void *recorder_ctx;

    /* COMPOSITION-2 commit binders (rx_world_set_binder); up to
     * RX_MAX_BINDERS. bind_mask[slot]: bound fields of that object;
     * bind_owner[slot]: index into binder[] of the binder owning them. */
    struct {
        RxBindCheckFn check;
        RxBindFn bind;
        RxBindAbortFn abort_fn;
        void *ctx;
    } binder[RX_MAX_BINDERS];
    uint32_t n_binders;
    uint8_t bind_mask[RX_MAX_OBJECTS];
    uint8_t bind_owner[RX_MAX_OBJECTS];
} RxWorld;

int  rx_world_init(RxWorld *w, RxCapRoot *root, uint32_t n_workers, uint64_t crumb_cap);
/* `root` may be null when `validate` is set. The Linux oracle is unchanged
 * when `validate` is null. */
int  rx_world_init_with_auth(RxWorld *w, RxCapRoot *root, const void *auth_ctx,
                             RxAuthValidateFn validate, RxAuthInspectFn inspect,
                             uint32_t n_workers, uint64_t crumb_cap);
int  rx_world_validate_cap(const RxWorld *w, RxCapRef ref, uint32_t subject,
                           uint64_t resource, uint32_t rights, RxCapEntry *out);
int  rx_world_inspect_cap(const RxWorld *w, RxCapRef ref, RxCapEntry *out);
struct AienosCapView;
int  rx_world_init_native(RxWorld *w, const struct AienosCapView *view,
                          uint32_t n_workers, uint64_t crumb_cap);
void rx_world_destroy(RxWorld *w);
/* Install the durable recorder. With `replay`, every crumb already in the
 * log is handed to it first, under the same lock, so nothing recorded before
 * installation is missed. RX_ERR_EXISTS if one is installed. `release` (may
 * be null) is called by rx_world_destroy if the recorder is still installed. */
int  rx_world_set_recorder(RxWorld *w, RxRecordFn fn, RxRecordReleaseFn release, void *ctx,
                           bool replay);
/* Remove the recorder installed with `ctx` (RX_ERR_NOT_FOUND otherwise).
 * release is not called. */
int  rx_world_clear_recorder(RxWorld *w, void *ctx);
/* COMPOSITION-2: install a commit binder (RX_ERR_EXISTS if one with `ctx` is
 * installed, RX_ERR_FULL past RX_MAX_BINDERS) and name the fields it owns.
 * Binding a field of a stale object is RX_ERR_STALE_GEN; a field another
 * binder owns is RX_ERR_EXISTS; without a binder for `ctx`, RX_ERR_NOT_FOUND.
 * clear removes the binder of `ctx` and every field it bound. */
int  rx_world_set_binder(RxWorld *w, RxBindCheckFn check, RxBindFn bind, RxBindAbortFn abort_fn,
                         void *ctx);
int  rx_world_bind_field(RxWorld *w, void *ctx, RxObjRef obj, uint32_t field);
int  rx_world_clear_binder(RxWorld *w, void *ctx);
uint32_t rx_world_binder_count(RxWorld *w);
/* R15: record an RxTiming per activation into `buf` (cap entries; later ones
 * are dropped and counted in n_timing beyond cap). Null turns it off. */
void rx_world_set_timing(RxWorld *w, RxTiming *buf, uint64_t cap);
/* Samples stored and samples attempted since rx_world_set_timing. Returns
 * RX_ERR_FULL when attempted > stored: statistics from that buffer are
 * truncated and must not be reduced. */
int rx_world_timing_status(RxWorld *w, uint64_t *stored, uint64_t *attempted);
/* 1 in production builds; 0 only in the R15 measurement build that skips
 * causal digests (-DRX_MEASURE_NO_CAUSAL_DIGEST). */
extern const int rx_world_causal_digest_enabled;
void rx_world_set_resources(RxWorld *w, const RxResourceBudget *budget);
void rx_world_set_stability(RxWorld *w, const RxStabilityBudget *stability);

/* Object lifecycle. Creation writes a CREATE crumb. Retiring bumps the slot
 * generation, so every outstanding RxObjRef to it becomes stale. */
int  rx_world_create(RxWorld *w, uint32_t type, RxPersist persist, uint64_t resource,
                     const uint64_t init[RX_MAX_FIELDS], RxObjRef *out);
int  rx_world_retire(RxWorld *w, RxObjRef ref);

/* Register a reaction; subscriptions go into the dependency index. In a
 * world with bound callers, d->caller must be the credential issued for
 * d->subject, or the call returns RX_ERR_IDENTITY and registers nothing. */
int  rx_world_add_reaction(RxWorld *w, const RxReactionDesc *d, uint32_t *out_id);
/* The same with d->caller taken from `keys` by d->subject (none when the
 * keyring has no entry). The caller's descriptor is not modified. */
int  rx_world_add_reaction_keyed(RxWorld *w, const RxCallerKeyring *keys,
                                 const RxReactionDesc *d, uint32_t *out_id);
/* Take a reaction out of the World. Allowed only while it is DORMANT with
 * nothing pending (not re-armed, parked, deferred, resume-pending, holding
 * resources, nor waiting in the fan-out backlog): RX_ERR_BUSY otherwise,
 * nothing changed. Its subscriptions leave the dependency index, it is never
 * woken or run again (demand ignores it), and its table slot is kept for
 * reuse: rx_world_add_reaction fills a removed slot before growing the table,
 * but only for a reaction with the same subject and faculty, so a crumb's
 * reaction id keeps naming the principal that wrote it
 * (rx_world_crumb_origin). RX_ERR_NOT_FOUND if out of range or removed. */
int  rx_world_remove_reaction(RxWorld *w, uint32_t rid);

/* What the World holds right now, for leak checks (read under the lock).
 * The causal crumb log is history (append-only by design) and is not here. */
typedef struct {
    uint32_t reaction_slots;     /* n_reactions: table slots ever used */
    uint32_t reactions_active;   /* registered, not removed */
    uint32_t reactions_removed;  /* removed slots waiting for reuse */
    uint64_t subscriptions;      /* entries in the dependency index */
    uint32_t objects_live;
    uint32_t binders;
    uint32_t bound_fields;
    uint32_t in_flight;
    uint32_t fanout_backlog;
    uint32_t deferred;
    uint32_t used_slots;
    uint64_t used_memory, used_energy;
} RxFootprint;
void rx_world_footprint(RxWorld *w, RxFootprint *out);

/* R16 C5 caller credentials (rx_caller.h).
 * enroll: mint a credential for `subject` (RX_CALLER_ERR_EXISTS if it has
 *   one, RX_CALLER_ERR_CLOSED once bound, RX_CALLER_ERR_FULL past
 *   RX_CALLER_MAX). The secret is returned only here.
 * bind: from now on every reaction registration, activation and commit
 *   checks the subject's credential. One way; enrollment closes. Refused
 *   (RX_ERR_IDENTITY, world left unbound) while any registered reaction was
 *   admitted without a credential (C7): before binding, a reaction that names
 *   a credential is fully checked, and one that names none is kept as
 *   unauthenticated and can never run in a bound world.
 * revoke: retire `subject`'s enrollment; needs its current credential, so no
 *   caller can revoke another's identity. Its reactions are then blocked.
 *   Takes the world lock (C7), so it never lands between a commit's identity
 *   check and its publish: it precedes the check or follows the publish.
 * check: RX_CALLER_OK or the RX_CALLER_ERR_* reason. Constant-time compare.
 * check_fn: the same, shaped for rx_gen_bind_authority (ctx is the world),
 *   with `op` one of RX_CALLER_OP_CHECK, _HOLD (on success the enrollment
 *   table stays locked, so no revocation lands, until _RELEASE). */
int  rx_world_enroll_caller(RxWorld *w, uint32_t subject, RxCallerCred *out);
int  rx_world_bind_callers(RxWorld *w);
int  rx_world_revoke_caller(RxWorld *w, uint32_t subject, const RxCallerCred *cred);
int  rx_world_check_caller(RxWorld *w, uint32_t subject, const RxCallerCred *cred);
int  rx_world_caller_check_fn(void *world, uint32_t subject, const RxCallerCred *cred, int op);

/* ---- R16 G6 operator emergency stop (spec/r16-operator-emergency-stop.md) ----
 * Authority: the caller credential of `subject` (rx_world_enroll_caller) AND a
 * capability for `subject` on RX_WORLD_RES_CONTROL carrying RX_WORLD_RIGHT_HALT,
 * validated by the world's authority. A subject any registered reaction runs
 * under is refused: no reaction can stop or resume the world it runs in.
 * Stop: no worker takes work, a computed activation is refused at its commit
 * (CANCELLED, RX_ERR_HALTED) and re-run on resume, outside publications and
 * seat results are refused, object create and retire are refused, and the
 * caller check refuses so a bound generation store neither proposes nor
 * promotes. A refused activation does not count toward the episode budget. Work already committing
 * finishes first (the stop waits for the world lock). A durable mark is
 * written when a halt directory is set; the stop takes effect even if that
 * write fails (`durable` < 0 says so). Never call with mu held. */
#define RX_WORLD_RES_CONTROL  0x906ull   /* the world's control resource */
#define RX_WORLD_RIGHT_HALT   RX_RIGHT_EPOCH /* the right that may void every
                                           capability may also pause the world */
#define RX_HALT_ALREADY       1   /* stop: already stopped, nothing changed */
#define RX_HALT_NOT_STOPPED   2   /* resume: not stopped, nothing changed */
int  rx_world_set_halt_dir(RxWorld *w, const char *dir);
int  rx_world_emergency_stop(RxWorld *w, uint32_t subject, const RxCallerCred *cred,
                             RxCapRef cap, int32_t reason, RxHaltStatus *out);
int  rx_world_emergency_resume(RxWorld *w, uint32_t subject, const RxCallerCred *cred,
                               RxCapRef cap, RxHaltStatus *out);
void rx_world_halt_status(RxWorld *w, RxHaltStatus *out);
/* The same authority check as stop and resume (credential, not a reaction
 * subject, live control capability), without acting: RX_OK, RX_ERR_IDENTITY
 * or RX_ERR_AUTHORITY. For operator requests that read or retire the
 * operator's own authority (docs/r16-operator-control.md). Never call with mu held. */
int  rx_world_operator_authorize(RxWorld *w, uint32_t subject, const RxCallerCred *cred, RxCapRef cap);

/* A stimulus from outside the organism (sensor, human input). Requires a
 * capability for (external_subject, object resource, WRITE). Returns the
 * causal id of the EXTERNAL crumb, or a negative error. */
int64_t rx_world_publish_external(RxWorld *w, RxCapRef cap,
                                  const RxMutation *muts, uint32_t n);

/* Wait until no reaction is READY/RUNNING/PUBLISHING. Observation only. */
int  rx_world_wait_quiescent(RxWorld *w, int timeout_ms);

/* Read-only inspection (takes the world lock). */
int  rx_world_read(RxWorld *w, RxObjRef ref, RxObject *out);
const RxCrumb *rx_world_crumb(const RxWorld *w, uint64_t id);
/* Who published crumb `id`: its reaction (UINT32_MAX for outside), that
 * reaction's subject (the external subject for outside) and faculty. RX_OK
 * for a commit or an outside publication, 1 for the object's creation (the
 * field still holds its initial value), negative otherwise. Takes the world
 * lock. */
int  rx_world_crumb_origin(RxWorld *w, uint64_t id, uint32_t *reaction, uint32_t *subject,
                           uint32_t *faculty);
/* Causal id of the publication that last wrote obj.field. */
uint64_t rx_world_explain(RxWorld *w, RxObjRef ref, uint32_t field);
/* Recompute every crumb digest and check parent links; 0 on success. */
int  rx_world_verify_crumbs(RxWorld *w, uint64_t *out_checked);
/* Digest a crumb exactly as the log does (parents are read from w). Binds
 * every capability reference in full, including all 64 generation bits. */
void rx_world_crumb_digest(const RxWorld *w, const RxCrumb *k, uint8_t out[32]);
/* SHA-256 over all live objects' (id, generation, content digest). */
void rx_world_digest(RxWorld *w, uint8_t out[32]);

/* Record which capability reference speaks for this object. Does not grant rights. */
int  rx_world_bind_capability(RxWorld *w, RxObjRef ref, RxCapRef cap);

/* Copy the 32-byte physical projection. Generation is the canonical one.
 * RX_ERR_UNPLACED means this generation has no realization. */
int  rx_world_physical(RxWorld *w, RxObjRef ref, OmegaSharedWorldObject *out);

/* Bind or drop the coherent window for this generation. Neither call
 * allocates an id or advances the generation. A second attach is refused.
 * A reference whose generation is not current is refused and changes nothing. */
int  rx_world_attach_physical(RxWorld *w, RxObjRef ref);
int  rx_world_detach_physical(RxWorld *w, RxObjRef ref);

/* Move the bytes of an attached object onto another free window.
 * The id, generation, version, and content digest stay put. */
int  rx_world_relocate_physical(RxWorld *w, RxObjRef ref);

/* Place the object at an exact region offset. Used to refuse a window that
 * does not fit. A refused call leaves the object where it was. */
int  rx_world_place_physical(RxWorld *w, RxObjRef ref, uint64_t offset, uint64_t length);

/* Hostile-writer seam: store bytes into the physical projection only.
 * Does not change the canonical object, its generation, or its capability. */
int  rx_world_overwrite_physical(RxWorld *w, uint32_t id, const OmegaSharedWorldObject *src);

/* Validate one descriptor against the canonical object and the capability root.
 * expected_sequence is the next ring sequence this descriptor must carry.
 * *out_fault receives a mailbox or RX_FAULT_* code. */
int  rx_world_check_descriptor(RxWorld *w, const OmegaSharedWorldDesc *desc,
                               uint64_t expected_sequence, uint32_t *out_fault);

/* Copy the next publication-ring slot and validate it. A hostile slot is
 * consumed and rejected so the ring still moves forward. */
int  rx_world_take_publication(RxWorld *w, OmegaSharedWorldDesc *out, uint32_t *out_fault);

/* Place a raw descriptor on the publication ring without correcting it.
 * The next take decides whether it is acceptable. */
int  rx_world_inject_descriptor(RxWorld *w, const OmegaSharedWorldDesc *desc);

/* Next publication sequence a producer would stamp. */
uint64_t rx_world_publication_tail(RxWorld *w);

/* Byte offset of physical record 0 inside the coherent image. */
uint64_t rx_world_physical_table_offset(void);

/* Fill checksum and the frozen magic/version. Does not invent an identity. */
void rx_world_seal_descriptor(OmegaSharedWorldDesc *desc);

/* Allow a resident seat on this image. The seat's one operation is fixed:
 * it adds field 0 and field 1 of the object it reads (low 32 bits, the same
 * integer add the qualified vector add uses) and writes the sum into field 0
 * of a second object. The notice names both objects. It does not carry code. */
int  rx_world_enable_resident(RxWorld *w);

/* Stand-in for the graphics seat, on the same image. One call consumes
 * notices until it has handled one claim, a shutdown, or the ring is empty.
 * Returns 1 if it wrote a result notice, 0 if there was nothing to do,
 * 2 if it saw shutdown, negative on a refused claim (a fault notice was
 * still published). */
int  rx_resident_seat_step(RxWorld *w);

/* Caller already holds the world lock. Posts one claim: read placed object
 * `in`, write placed object `out`. The notice names both objects, their
 * generations, and their bound capabilities. It does not carry code. */
int  rx_resident_post_claim(RxWorld *w, uint32_t in, uint32_t out, uint64_t parent,
                            uint64_t *seq_out);

/* Clear the seat's stopped flag. Does not move either ring. */
int  rx_resident_reset(RxWorld *w);

/* The durable executor finished the effect reaction `reaction` deferred on
 * (RX_FN_DEFER): run that activation again. Safe from any thread. Decides
 * nothing; the reaction re-reads the world and the executor's result. */
int  rx_world_resume(RxWorld *w, uint32_t reaction);

/* Take one result notice and, when it is valid, publish it into the canonical
 * object. A dependent reaction wakes from that publication. */
int  rx_resident_accept(RxWorld *w);

/* Block until at least one resident claim is outstanding (posted and not yet
 * closed) or timeout_ms passes. Transport only: it decides nothing. Returns
 * RX_OK when work is outstanding, RX_ERR_TIMEOUT otherwise. */
int rx_resident_wait_outstanding(RxWorld *w, int timeout_ms);

/* Ask the seat to leave. The object world stays. */
int  rx_resident_shutdown(RxWorld *w);

/* The seat is gone without leaving: killed, or its channel reset. The caller
 * guarantees the chip no longer writes the image. Claims the old seat never
 * took and results nobody accepted are discarded, and the seat generation
 * moves. Every claim the seat still held ends FAILED with RX_ERR_SEAT_LOST:
 * the output window is restored from the canonical object, the resource
 * charge is released, and a crumb records the loss. With retry, each of those
 * activations is made ready again, caused by its failure crumb, and its new
 * claim carries the new seat generation. Returns the number of claims lost. */
int  rx_resident_seat_lost(RxWorld *w, int retry);
/* Caller holds the world lock. Copies one graphics-to-processor result and
 * moves that ring forward. */
int  rx_resident_take_result(RxWorld *w, OmegaSharedWorldDesc *out);

/* Point the one image at caller memory of the same size. borrowed means the
 * world will not free it. */
int  rx_world_bind_coherent(RxWorld *w, void *mem, uint64_t bytes, int borrowed);

uint64_t rx_world_off_c2g(void);
uint64_t rx_world_off_g2c(void);
uint64_t rx_world_off_fault(void);
uint64_t rx_world_off_object_table(void);
uint64_t rx_world_off_heartbeat(void);

/* Caller holds the world lock. Projection and publication of the same object. */
int  rx_coherent_format(RxWorld *w);
void rx_coherent_free(RxWorld *w);
void rx_coherent_project(RxWorld *w, uint32_t id);
void rx_coherent_publish(RxWorld *w, uint32_t id, uint64_t crumb_id);

bool rx_state_transition_legal(RxState from, RxState to);
const char *rx_state_name(RxState s);
const char *rx_crumb_kind_name(RxCrumbKind k);

#endif /* RX_WORLD_H */
