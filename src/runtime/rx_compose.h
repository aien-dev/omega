/*
 * rx_compose.h -- COMPOSITION-2: one World, one causal path.
 *
 *   goal (outside input, with the routed Skill alternatives)
 *     -> compose.candidate.k: fork a staged J-Space branch from the committed
 *        state, run the routed Skill, derive the branch with its result
 *     -> compose.verify (AEGIS faculty): contract check of every candidate,
 *        picks one winner or none
 *     -> compose.commit: names the winner in state.field[0]
 *     -> World commit binder: the new branch must be live, staged, local and
 *        a child of the branch the World names now; then it is sealed (the
 *        point of no return) and the World publishes. Any refusal before the
 *        seal rejects the commit and releases the branch.
 *     -> settle (driver, at quiescence): reclaim every staged loser, make the
 *        sealed winner durable (js_space_commit), record the composition in
 *        Cortex (candidates, verification evidence, one promotion, one
 *        admission per loser), release the superseded branch.
 *
 * Routing discovers (rx_skillroute, no mint). Authority is minted here, at
 * open, by the system owner through the AIENOS admin: each reaction holds
 * only the rights its step needs, so a candidate cannot write the state and
 * only the AEGIS verifier can write the verdict.
 *
 * Recovery (OLD-or-NEW). On open the state is the newest Cortex record of
 * the state object (ENTITY_CREATED or EXEC_COMMIT) whose branch is live and
 * sealed in the reopened durable J-Space checkpoint. A newer record whose
 * branch did not become durable is answered with a CX_K_ADMISSION rollback
 * record (tag RXC_ADMIT_ROLLBACK). Staged branches are never persisted, so a
 * loser can never be recovered.
 */
#ifndef RX_COMPOSE_H
#define RX_COMPOSE_H

#include "aien_machine_id.h"
#include "aienos_cap.h"
#include "rx_cortex.h"
#include "rx_jspace.h"
#include "rx_skillroute.h"
#include "rx_world.h"

#define RXC_K 2u                      /* alternatives explored per goal */
#define RXC_UNIT 4096u
#define RXC_CRASH_EXIT 77             /* exit code of an injected crash */
#define RXC_NONE 0xFFFFFFFFu          /* verdict: no winner */

/* World object slots, created in this order at every open; the Cortex
 * subject of a World record is slot + 1 (rx_cortex_record.h). Composition
 * records (candidates, evidence, promotion, admissions) are about the state
 * object, so they share its subject. */
enum { RXC_SLOT_GOAL = 0, RXC_SLOT_CAND0, RXC_SLOT_CAND1, RXC_SLOT_VERDICT, RXC_SLOT_STATE };
#define RXC_CX_SUBJECT(slot) ((uint64_t)(slot) + 1u)

/* Fault points on the path, in causal order. */
enum {
    RXC_FP_NONE = 0,
    RXC_FP_BEFORE_FORK,       /* candidate reaction, before js_branch_fork_staged */
    RXC_FP_CANDIDATE,         /* candidate reaction, after fork + derive, before publish */
    RXC_FP_BEFORE_COMMIT,     /* commit reaction, before it proposes the winner */
    RXC_FP_AFTER_VALIDATION,  /* World binder check, after authority and version checks */
    RXC_FP_SEAL,              /* World binder: crash right after the seal / seal refused */
    RXC_FP_RECLAIM,           /* settle: after loser reclaim, before js_space_commit */
    RXC_FP_CORTEX,            /* settle: between composition Cortex records */
    RXC_FP_END
};

/* Admission tags (CX_K_ADMISSION.tag). */
enum { RXC_ADMIT_LOSER = 1, RXC_ADMIT_ROLLBACK = 2, RXC_ADMIT_RECOVERED = 3 };

/* Composition record payloads (subject RXC_CX_SUBJECT(RXC_SLOT_STATE)).
 *   CX_K_CANDIDATE  (CLAIM)     tag = Cortex id of the goal record;
 *                               links {candidate World record, goal record}
 *   CX_K_EVIDENCE_REF (EVIDENCE, VERIFY_EVIDENCE) tag = goal record;
 *                               links {verdict World record, claim 0, claim 1,
 *                               state World record or 0}
 *   CX_K_PROMOTION  cx_promote(winner claim, evidence)
 *   CX_K_ADMISSION  tag = RXC_ADMIT_*; LOSER links {loser claim, evidence},
 *                   ROLLBACK links {refused state record}, RECOVERED links
 *                   {state record, evidence} */
enum { RXC_CP_K = 0, RXC_CP_REF, RXC_CP_RESULT, RXC_CP_SKILL, RXC_CP_INPUT, RXC_CP_GOALSEQ,
       RXC_CP_PASS, RXC_CP_DIGEST0, RXC_CP_SKILLDIG0 = RXC_CP_DIGEST0 + 4,
       RXC_CP_HOME = RXC_CP_SKILLDIG0 + 4,      /* RXC_HOME_*: where the Skill ran */
       RXC_CP_WORDS };
enum { RXC_EP_WINNER = 0, RXC_EP_WREF, RXC_EP_LREF, RXC_EP_PASSMASK, RXC_EP_RESULT,
       RXC_EP_GOALSEQ, RXC_EP_VERIFIER, RXC_EP_WDIGEST0, RXC_EP_WORDS = RXC_EP_WDIGEST0 + 4 };
enum { RXC_AP_REF = 0, RXC_AP_GOAL, RXC_AP_K, RXC_AP_WORDS };

/* Verdict object fields. */
enum { RXC_V_WINNER = 0, RXC_V_WREF, RXC_V_LREF, RXC_V_PASSMASK, RXC_V_GOAL, RXC_V_RESULT };
/* Candidate object fields. */
enum { RXC_C_REF = 0, RXC_C_RESULT, RXC_C_SKILL, RXC_C_GOAL, RXC_C_DONE, RXC_C_HOME };
/* Where a candidate's Skill ran (candidate RXC_C_HOME, claim RXC_CP_HOME). */
enum { RXC_HOME_LOCAL = 0, RXC_HOME_FABRIC = 1 };
/* Goal object fields. */
enum { RXC_G_INPUT = 0, RXC_G_OP, RXC_G_SKILL0, RXC_G_SKILL1, RXC_G_SEQ };
/* State object fields. */
enum { RXC_S_REF = 0, RXC_S_RESULT, RXC_S_GOAL };

typedef enum {
    RXC_SUBJ_EXTERNAL = 200, RXC_SUBJ_CAND0 = 201, RXC_SUBJ_CAND1 = 202,
    RXC_SUBJ_AEGIS = 203, RXC_SUBJ_COMMIT = 204
} RxcSubject;

#define RXC_RES_GOAL    0xC2000001ull
#define RXC_RES_CAND0   0xC2000002ull
#define RXC_RES_CAND1   0xC2000003ull
#define RXC_RES_VERDICT 0xC2000004ull
#define RXC_RES_STATE   0xC2000005ull
/* Instances: up to RXC_MAX_ACTIVE compositions share one World; instance i
 * offsets the role subjects by RXC_SUBJ_STRIDE*i and the resources by
 * RXC_RES_STRIDE*i (instance 0 = the values above). */
#define RXC_MAX_ACTIVE 2u
#define RXC_SUBJ_STRIDE 8u
#define RXC_RES_STRIDE 0x100ull
#define RXC_SUBJ_OF(inst, role) ((uint32_t)(role) + (uint32_t)(inst) * RXC_SUBJ_STRIDE)
#define RXC_RES_OF(inst, res) ((uint64_t)(res) + (uint64_t)(inst) * RXC_RES_STRIDE)

/* The contract the AEGIS verifier enforces on a candidate's result. */
typedef int (*RxcContract)(uint64_t input, uint64_t result);

/* Fabric: how a candidate runs a route whose provider is on another machine
 * (route->verdict == SR_E_REMOTE). Called from the candidate reaction, on a
 * World worker, with the run's clock. It must revalidate the route against
 * the Capability Graph (sr_route_check == SR_E_REMOTE) and the machine's
 * Fabric membership at now_us, run exactly the procedure the route names
 * (skill id + advertised digest) on that machine, and return 0 with its
 * result, or nonzero to refuse (the candidate then proposes nothing). It
 * mints and holds no World authority: the result is only a candidate, checked
 * by the AEGIS verifier like any local one. Two candidates may call it at
 * once. Without a hook a remote route proposes nothing (as before). */
typedef int (*RxcRemoteRun)(void *ctx, const SrRouter *router, const SrRoute *route,
                            uint64_t input, uint64_t now_us, uint64_t *result);

typedef struct {
    int outcome;                       /* RXC_OUT_* */
    uint32_t n_alternatives;
    SrRoute route[RXC_K];
    uint32_t winner;                   /* index into route[], UINT32_MAX = none */
    JsBranchRef old_ref, new_ref, cand_ref[RXC_K];
    uint64_t result;
    uint64_t cx_candidate[RXC_K], cx_evidence, cx_promotion, cx_admission[RXC_K];
    uint64_t goal_crumb;               /* EXTERNAL crumb of the goal */
    uint32_t reclaimed;                /* staged branches reclaimed at settle */
    uint8_t winner_digest[32];         /* J-Space content digest of the committed branch */
    uint32_t prior_completed;          /* records of an earlier failed record completed first */
} RxcResult;

enum {
    RXC_OUT_COMMITTED = 1,             /* NEW is committed and durable */
    RXC_OUT_NO_WINNER,                 /* every candidate failed the contract: OLD kept */
    RXC_OUT_NOT_COMMITTED,             /* the path stopped before the World commit: OLD kept */
    RXC_OUT_NOT_DURABLE,               /* World named NEW, J-Space did not make it durable */
    RXC_OUT_RECORD_FAILED              /* NEW durable; composition record incomplete */
};

typedef struct RxCompose {
    char dir[200];
    AienMachineId self;
    RxWorld w;                         /* storage of the World rx_compose_open owns */
    RxWorld *world;                    /* the World it runs in: &w (open) or the caller's (attach) */
    int owns_world;                    /* 1: open built it and close destroys it */
    const RxCallerKeyring *keys;       /* attach: credentials of the composition subjects */
    JsSpace js;
    CxStore cx;
    const SrRouter *router;
    RxcContract contract;
    AienosCapAdmin *admin;
    uint64_t session;
    RxObjRef goal, cand[RXC_K], verdict, state;
    uint32_t rx_cand[RXC_K], rx_verify, rx_commit;
    uint32_t n_rx;                     /* of those, registered so far (cand 0, cand 1, verify, commit) */
    uint64_t seq;
    /* recovery report of the last open */
    JsBranchRef recovered;
    uint64_t recovered_record;         /* Cortex id naming it */
    uint32_t rolled_back;              /* newer records refused (rollback admissions) */
    uint32_t recovered_completed;      /* composition records completed at open (0 = none missing) */
    /* in-process state after RECORD_FAILED / NOT_DURABLE (cleared at open) */
    int pending;                       /* 0 none; 1 record to complete first; 2 refuse: reopen */
    int pend_released;                 /* the superseded branch was already released */
    uint64_t pend_S, pend_V;
    JsBranchRef pend_old;
    uint8_t pend_cdig[RXC_K][32];
    /* per-run scratch read by reactions */
    uint64_t run_input;
    uint32_t n_routes;
    SrRoute run_route[RXC_K];
    uint64_t run_now;                  /* the run's clock (remote dispatch revalidates at it) */
    /* Fabric dispatch of remote routes (rx_compose_set_remote; cleared at open/attach). */
    struct {
        RxcRemoteRun run;
        void *ctx;
        uint64_t ran, refused;         /* remote candidate runs: done / refused by the hook */
        uint64_t seq[RXC_K];           /* goal seq of the last remote run of candidate k */
        uint8_t digest[RXC_K][32];     /* the advertised digest it ran */
    } remote;
    /* authority minted at open (one per step, rights of that step only) */
    RxCapRef cap_ext, cap_cand[RXC_K][3], cap_verify[4], cap_commit[2];
    struct RxcCandUser { struct RxCompose *c; uint32_t k; } cand_user[RXC_K];
    int attached;                      /* its Cortex link is attached to the World */
    /* what open/attach put into the World (attach close undoes it) */
    int has_binder;
    uint32_t n_objs, n_minted;
    /* instance slot in its World (attach; 0 under open) and what it implies */
    uint32_t inst;
    uint32_t subj_cand[RXC_K], subj_aegis, subj_commit;
    uint64_t res[5];
    RxObjRef obj[5];
    /* Test hooks. They fire only in builds compiled with -DRXC_TEST_HOOKS
     * (the composition unit test and the R13 host test); in every other
     * build they are ignored and the _exit path does not exist. */
    struct RxcTestHooks {
        int fault_point;               /* RXC_FP_* */
        int fault_crash;               /* 1: _exit(RXC_CRASH_EXIT); 0: fail in process */
        uint32_t fault_k;              /* candidate index for candidate-side points */
        int fault_hit;
        int rogue_candidate;           /* candidate 0 also proposes a state write */
        /* Hold candidate hold_k1 - 1 (0: none) once, after its Skill ran and
         * before it proposes: it sets held, waits for release, then sets
         * hold_done and returns its proposal for the World to publish. */
        uint32_t hold_k1;
        int held, release, hold_done;
    } test;
} RxCompose;

/* Open (or create) the composition in `dir`: machine identity must match the
 * stored one (RX_ERR_IDENTITY otherwise), J-Space and the Cortex journal are
 * reopened (a torn journal tail is repaired, then the chain is verified), the
 * World is built with `n_workers` (the composition owns it; see
 * rx_compose_attach to run inside an existing World), state is recovered
 * OLD-or-NEW. */
int  rx_compose_open(RxCompose *c, const char *dir, const AienMachineId *self, uint64_t session,
                     const SrRouter *router, RxcContract contract, AienosCapAdmin *admin,
                     AienosCapView *view, uint32_t n_workers);
/* One goal through the whole path. Returns RX_OK with out->outcome set, or a
 * negative error (an injected in-process fault shows in outcome instead).
 * After RXC_OUT_RECORD_FAILED the next run first completes that record (and
 * releases the superseded branch); if it cannot, it returns RX_ERR_REPLAY.
 * After RXC_OUT_NOT_DURABLE every run returns RX_ERR_REPLAY until reopened. */
int  rx_compose_run(RxCompose *c, uint64_t input, const SrRequirement *req, const CqHeld *held,
                    uint64_t now_us, RxcResult *out);
void rx_compose_close(RxCompose *c);
/* COMPOSITION-2 inside an existing World (the living one).
 *
 * enroll_callers: the World's owner enrolls the four composition reaction
 * subjects of every instance (role + RXC_SUBJ_STRIDE*i for i < RXC_MAX_ACTIVE: 8
 * subjects) into `w` and gets their
 * credentials in `keys`; it must run before rx_world_bind_callers (R16: the
 * enrollment closes one way at bind). RX_ERR_IDENTITY once bound.
 *
 * attach: like open, on `dir` and `self`, but the composition runs inside
 * `w` (which it neither builds nor destroys):
 *   - its five objects get whatever ids `w` has free; c->goal..c->state hold
 *     them. The Cortex journal in `dir` keeps the composition subjects
 *     RXC_CX_SUBJECT(slot) through a scoped Cortex link
 *     (rx_cortex_attach_scoped), so the record is the same as under open and
 *     survives a reopen in a World that hands out other ids; living crumbs
 *     never enter it, and the World's recorder slot stays free for (or
 *     shared with) the World's own Cortex;
 *   - w->external_subject is not changed: the goal capability is minted for
 *     the World's own external subject, on the composition goal resource only;
 *   - reactions are registered with `keys` (rx_world_add_reaction_keyed), so a
 *     World with bound callers admits them; without bound callers keys may be
 *     NULL;
 *   - it takes a free instance slot (below) and installs that instance's
 *     commit binder (one of the World's RX_MAX_BINDERS): RX_ERR_EXISTS if
 *     RXC_MAX_ACTIVE compositions already run in `w`, or one of them uses
 *     `dir` (the journal is single-owner); RX_ERR_FULL if the reaction table
 *     has no room for its four reactions. Either refusal changes nothing.
 * close (attach mode), in this order: (1) revokes every capability attach
 * minted, so no composition step can publish from here on (a step already
 * running finds its rights gone at publish and is REJECTED); (2) removes the
 * scoped Cortex link, so a refused late write never enters the composition
 * journal (the World's crumb log still records it); (3) waits until none of
 * the composition's OWN four reactions is READY, RUNNING, PUBLISHING,
 * BLOCKED_RESOURCE, re-armed, parked, deferred, or waiting in the World's
 * fan-out backlog (still DORMANT there): those still use the
 * RxCompose as their user pointer. There is no wall-clock cutoff and no wait
 * for the rest of the World: a living World may never be quiet as a whole,
 * and returning while a step still runs would leave it using freed memory.
 * A Skill that never returns therefore hangs close (the safe choice);
 * (4) removes its binder, retires the five objects and reclaims the revoked
 * capabilities (their AIENOS slots become free; the slot generation
 * advances, so the old references never validate again); (5) removes its
 * four reactions from the World (rx_world_remove_reaction): their
 * subscriptions go, the slots are marked removed and the next attach of the
 * same instance reuses them. It leaves `w` running.
 *
 * Invariant: close reclaims everything attach allocated. After close the
 * World's footprint (rx_world_footprint: active reactions, subscriptions,
 * live objects, binders, bound fields, in-flight work, backlog, resource
 * use) and the AIENOS capability table are what they were before attach; the
 * reaction table does not grow across attach/close cycles (a removed slot is
 * reused only by the same subject and faculty, so crumb provenance of old
 * runs still names the right subject). Only the crumb log grows: it is
 * history. Tested for 2000 cycles under ASan.
 *
 * Invariant: up to RXC_MAX_ACTIVE (2) compositions per World at a time, each
 * isolated. Instance i (0 or 1) uses subjects RXC_SUBJ_OF(i, role) and
 * resources RXC_RES_OF(i, res); instance 0 keeps 200..204 and
 * 0xC2000001..5, so its records are the ones a single composition always
 * wrote. Capabilities are scoped by subject and resource, so instance B's
 * steps hold no right on instance A's objects (a write naming them is
 * refused), each instance has its own binder on its own state object, its
 * own Cortex scoped link and journal, and its own step set; run waits only
 * for its own steps. The bound is the caller keyring: 2 instances x 4
 * subjects = RX_CALLER_KEYRING_MAX. Attaches are serialized process-wide;
 * runs and closes of different instances may overlap. After close the same
 * or another RxCompose can attach again and recovers OLD-or-NEW from its
 * directory. */
int  rx_compose_enroll_callers(RxWorld *w, RxCallerKeyring *keys);
int  rx_compose_attach(RxCompose *c, RxWorld *w, const RxCallerKeyring *keys, const char *dir,
                       const AienMachineId *self, uint64_t session, const SrRouter *router,
                       RxcContract contract, AienosCapAdmin *admin);
/* Install (run != NULL) or remove the Fabric dispatch of remote routes. Call
 * after open/attach (both clear it) and never during rx_compose_run. */
int  rx_compose_set_remote(RxCompose *c, RxcRemoteRun run, void *ctx);
/* The branch the World names now. */
JsBranchRef rx_compose_state(RxCompose *c);
/* Content digest of the composition record (Cortex objects without timing
 * words), stable across identical runs. */
void rx_compose_record_digest(const CxStore *s, uint8_t out[32]);
/* The deterministic realizer of the state unit. */
extern const JsRealizer RXC_REALIZER;

#endif /* RX_COMPOSE_H */
