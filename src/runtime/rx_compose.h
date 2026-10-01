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
       RXC_CP_WORDS = RXC_CP_SKILLDIG0 + 4 };
enum { RXC_EP_WINNER = 0, RXC_EP_WREF, RXC_EP_LREF, RXC_EP_PASSMASK, RXC_EP_RESULT,
       RXC_EP_GOALSEQ, RXC_EP_VERIFIER, RXC_EP_WDIGEST0, RXC_EP_WORDS = RXC_EP_WDIGEST0 + 4 };
enum { RXC_AP_REF = 0, RXC_AP_GOAL, RXC_AP_K, RXC_AP_WORDS };

/* Verdict object fields. */
enum { RXC_V_WINNER = 0, RXC_V_WREF, RXC_V_LREF, RXC_V_PASSMASK, RXC_V_GOAL, RXC_V_RESULT };
/* Candidate object fields. */
enum { RXC_C_REF = 0, RXC_C_RESULT, RXC_C_SKILL, RXC_C_GOAL, RXC_C_DONE };
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

/* The contract the AEGIS verifier enforces on a candidate's result. */
typedef int (*RxcContract)(uint64_t input, uint64_t result);

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
    RxWorld w;
    JsSpace js;
    CxStore cx;
    const SrRouter *router;
    RxcContract contract;
    AienosCapAdmin *admin;
    uint64_t session;
    RxObjRef goal, cand[RXC_K], verdict, state;
    uint32_t rx_cand[RXC_K], rx_verify, rx_commit;
    uint64_t seq;
    /* recovery report of the last open */
    JsBranchRef recovered;
    uint64_t recovered_record;         /* Cortex id naming it */
    uint32_t rolled_back;              /* newer records refused (rollback admissions) */
    uint32_t recovered_completed;      /* composition records completed at open (0 = none missing) */
    /* fault injection */
    int fault_point;
    int fault_crash;                   /* 1: _exit(RXC_CRASH_EXIT); 0: fail in process */
    uint32_t fault_k;                  /* candidate index for candidate-side points */
    int fault_hit;
    /* per-run scratch read by reactions */
    uint64_t run_input;
    uint32_t n_routes;
    SrRoute run_route[RXC_K];
    /* authority minted at open (one per step, rights of that step only) */
    RxCapRef cap_ext, cap_cand[RXC_K][3], cap_verify[4], cap_commit[2];
    struct RxcCandUser { struct RxCompose *c; uint32_t k; } cand_user[RXC_K];
    int attached;                      /* Cortex recorder attached to w */
    int test_rogue_candidate;          /* test hook: candidate 0 also proposes a state write */
} RxCompose;

/* Open (or create) the composition in `dir`: machine identity must match the
 * stored one (RX_ERR_IDENTITY otherwise), J-Space and the Cortex journal are
 * reopened (a torn journal tail is repaired, then the chain is verified), the
 * World is built with `n_workers`, state is recovered OLD-or-NEW. */
int  rx_compose_open(RxCompose *c, const char *dir, const AienMachineId *self, uint64_t session,
                     const SrRouter *router, RxcContract contract, AienosCapAdmin *admin,
                     AienosCapView *view, uint32_t n_workers);
/* One goal through the whole path. Returns RX_OK with out->outcome set, or a
 * negative error (an injected in-process fault shows in outcome instead). */
int  rx_compose_run(RxCompose *c, uint64_t input, const SrRequirement *req, const CqHeld *held,
                    uint64_t now_us, RxcResult *out);
void rx_compose_close(RxCompose *c);
/* The branch the World names now. */
JsBranchRef rx_compose_state(RxCompose *c);
/* Content digest of the composition record (Cortex objects without timing
 * words), stable across identical runs. */
void rx_compose_record_digest(const CxStore *s, uint8_t out[32]);
/* The deterministic realizer of the state unit. */
extern const JsRealizer RXC_REALIZER;

#endif /* RX_COMPOSE_H */
