/*
 * rx_r15_rig.h -- the R13 living body built for an R15 configuration
 * (spec/r15-performance-proof.md §3, §4). Shared by the SEQ parity gate and
 * the R15 harness so both drive exactly the same objects, reactions,
 * authority, crumbs, R9 store and seat.
 *
 * RES-4 / RES-1: rx_world_init_native with 4 / 1 workers; a transport thread
 *   moves seat completions into the world (it decides nothing).
 * SEQ: rx_world_init_native_sequential_reference; one orchestrator thread
 *   runs rx_seq_pulse over the §2.2 faculty order, rebuilt whenever a
 *   reaction is registered, and host-waits for each GPU claim.
 *
 * The harness side only publishes outside stimuli (placement, goal,
 * production requests) and reads objects. It never calls a faculty.
 */
#ifndef RX_R15_RIG_H
#define RX_R15_RIG_H

#include "runtime/rx_living.h"
#include "runtime/rx_resident_gpu.h"
#include "runtime/rx_seq_reference.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

typedef enum { R15_RES4, R15_RES1, R15_SEQ } R15Config;

#define R15_M 64u
#define R15_N 256u
#define R15_CLASS_A725 0xd87u
#define R15_CLASS_X925 0xd85u

typedef struct {
    R15Config config;
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxOmegaFaculty omega;
    RxAienFaculty aien;
    RxAegisFaculty aegis;
    RxLiving living;
    RxLivingPromoter promoter;
    RxGenStore *gen;
    RxGpuSeat *seat;
    RxObjRef intent;
    RxCapRef ext_request, ext_goal, ext_placement, ext_intent;
    RxCapRef seat_input, seat_output;
    uint32_t r_ask;
    uint64_t minted;
    char generation_dir[128];
    /* production client */
    uint64_t request_seq, served, wrong, unpromoted_use;
    pthread_t producer;
    atomic_int producer_stop, producer_error;
    int producer_live;
    char serve_why[320];        /* why the last failed request failed */
    /* RES: completion transport */
    pthread_t transport;
    atomic_int transport_stop, transport_error;
    int transport_live;
    /* SEQ: orchestrator */
    pthread_t orchestrator;
    atomic_int orch_stop, orch_error;
    int orch_live;
    uint32_t order[RX_MAX_REACTIONS];
    RxSeqPlan plan;
    uint32_t plan_for;          /* n_reactions the plan was built for */
    /* lost-trigger baseline, taken when the body is built */
    uint64_t baseline[RX_MAX_REACTIONS][RX_MAX_DEPS];
    int stage;                  /* setup stage reached, for diagnostics */
    const char *stage_why;      /* which step of that stage failed, if known */
    /* R15 harness: optional measurement points inside r15_episode (all NULL
     * for the parity gate, which then runs exactly as before). */
    const struct R15Hooks *hooks;
    int64_t goal_crumb;         /* crumb of the goal publication */
} R15Rig;

/* Called on the episode's thread; a nonzero return fails the episode.
 *   idle:   A725, confirmed, producer not yet running (idle baseline);
 *   before: A725, producer running (BEFORE window);
 *   after:  X925, goal MET on the in-force record, producer still running
 *           (AFTER window). */
typedef struct R15Hooks {
    int (*idle)(struct R15Hooks *h, R15Rig *r);
    int (*before)(struct R15Hooks *h, R15Rig *r);
    int (*after)(struct R15Hooks *h, R15Rig *r);
    void *ctx;
} R15Hooks;

/* Semantic outcome of one W-EPISODE. Exact fields must agree between
 * configurations; `measured` fields are timing and are only reported. */
typedef struct {
    int ok;                     /* the episode ran to goal MET */
    char why[480];
    uint64_t target_ns;
    /* exact */
    uint64_t goal_seq, goal_regime, goal_status, goal_class;
    uint64_t goal_status_final;  /* after the AFTER window (hooks only); reported */
    uint64_t plan_action, plan_regime, plan_condition, plan_reason, plan_goal;
    uint64_t search_epoch, selection_epoch, selection_id[4], selection_regime;
    uint64_t selected_verdict;  /* verdict state of the slot that produced it */
    uint64_t evidence[5];
    uint64_t belief[4];
    uint64_t gen_before, gen_after, lineage_before, lineage_after;
    uint64_t inforce_epoch, inforce_id[4], inforce_regime, inforce_generation;
    int64_t promote_result;
    uint64_t candidate_id, promotion_candidate, promotion_active;
    uint64_t aegis_woken, root_woken;
    uint64_t gpu_claims, seat_commits;
    /* integrity */
    uint64_t crumbs_checked, crumb_overflow, illegal, wrong, unpromoted_use;
    uint64_t lost_triggers;
    int crumbs_verified;
    /* measured (reported only) */
    uint64_t incumbent_ns, selected_ps, reference_ps, inforce_ps, final_expected_ns;
    uint64_t plan_seq, served, crumbs;
    /* per Omega slot, last search epoch (reported only): verdict state,
     * measure state, candidate ps, reference ps measured in the same rounds */
    uint64_t slot_verdict[RX_OMEGA_SLOTS], slot_measure[RX_OMEGA_SLOTS];
    uint64_t slot_cps[RX_OMEGA_SLOTS], slot_rps[RX_OMEGA_SLOTS];
} R15Outcome;

int  r15_start(R15Rig *r, R15Config config);
/* Runs the §4 episode shape without measurement windows: A725 warm-up until
 * Omega's first selection and AIEN's confirmed prediction; production
 * running; X925 placement and goal; until promotion; until AIEN assesses the
 * goal MET on the in-force record; production stopped; quiescent.
 * target_ns = 0 derives the goal from AIEN's confirmed cost (TARGET 55%);
 * otherwise it is used as given (the parity gate gives both sides one). */
int  r15_episode(R15Rig *r, uint64_t target_ns, R15Outcome *out);
void r15_stop(R15Rig *r);
/* Mint a grant from the rig's authority (harness stimuli only). */
RxCapRef r15_mint(R15Rig *r, uint32_t subject, uint64_t resource, uint32_t rights);
/* The closed-loop production client, outside an episode (L1/L2 only). */
int r15_producer_start(R15Rig *r);
int r15_producer_stop(R15Rig *r);       /* -1 if a request failed */
int r15_placement(R15Rig *r, uint32_t cls);
/* Confine every thread of the process to one core class. */
int r15_move_class(uint32_t cls);
const char *r15_config_name(R15Config c);

/* Reactions whose last activation read an older version of a trigger field
 * than the one now in the world (changes after the build baseline), in a
 * quiescent world. Such a trigger was lost. Quarantined reactions and
 * retired trigger objects are exempt and not counted. */
uint64_t r15_lost_triggers(R15Rig *r, char *first, size_t n);

#endif
