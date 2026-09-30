/*
 * rx_living.h -- R13: the faculties as one resident causal system (ADR 0016).
 *
 * Nothing here sequences the faculties. These are five more reactions on the
 * caller's one RxWorld, each made ready only by the state it reads:
 *
 *   living.experiment.prepare  Omega search epoch taken up for an AIEN plan
 *                              -> GPU input, trial 0
 *   living.blackwell.add       GPU input -> GPU output field 0
 *                              (the resident R12 seat; R5 admission, R7
 *                              authority read from an R8 capability slot)
 *   living.experiment.evidence GPU output -> checked sum; the next trial
 *                              until RX_LIVING_TRIALS, then evidence
 *   generation.prepare         Omega selection + AIEN experiment belief
 *                              -> R9 draft proposed, candidate object
 *   generation.promote         candidate object -> rx_gen_promote under a
 *                              separate subject; on success the in-force
 *                              record production reads
 *   generation.restore         (R14) body started -> the active R9 generation's
 *                              bytes, verified again by Omega -> in-force record
 *
 * AIEN's belief about the evidence (rx_aien_register_experiment) and Omega's
 * wait for that belief (rx_omega_require_evidence) live in those faculties.
 * Production reads the in-force record (rx_omega_serve_from): Omega's
 * selection is only eligible until the promotion authority promotes it.
 *
 * The GPU operation is R12's fixed add of fields 0 and 1 (low 32 bits). It is
 * a physical witness of the experiment and its authority path. It is not the
 * matvec realization and does not measure matvec cost.
 */
#ifndef RX_LIVING_H
#define RX_LIVING_H

#include "rx_aegis.h"
#include "rx_aien.h"
#include "rx_omega.h"
#include "rx_generation.h"

/* Objects in the caller's RxWorld. Not a second world, not a device namespace.
 * input:     0 operand a, 1 operand b, 2 search epoch it is for, 3 trial,
 *            4 plan seq
 * output:    0 low 32 bits of input 0 + input 1                  (GPU seat)
 * evidence:  0 epoch (set when the experiment ends), 1 expected, 2 observed
 *            (last trial), 3 every trial matched (1), 4 trials matched
 * candidate: 0 R9 candidate id, 1 epoch, 2..5 realization id, 6 regime,
 *            7 selected ps per call                               (prepare)
 * promotion: 0 candidate id taken up, 1 rx_gen_promote result (signed),
 *            2 barrier ns, 3 active generation after              (promote)
 * inforce:   selection layout: 0 epoch, 1..4 realization id, 5 ps per call,
 *            6 regime, 7 generation it came from                  (promote) */
/* Physical trials per experiment. Each is one claim on the resident seat
 * under the same live grant. */
#define RX_LIVING_TRIALS 16u
#define RX_LIVING_RES_BASE 0x6130000ull
enum { RX_LIVING_RES_INPUT, RX_LIVING_RES_OUTPUT, RX_LIVING_RES_EVIDENCE,
       RX_LIVING_RES_CANDIDATE, RX_LIVING_RES_PROMOTION, RX_LIVING_RES_INFORCE,
       RX_LIVING_RES_RESTORE };
enum { RX_LIVING_SUBJ = 61, RX_LIVING_SEAT_SUBJ = 62,
       RX_LIVING_PREPARE_SUBJ = 63, RX_LIVING_PROMOTE_SUBJ = 64 };
enum { RX_OT_LIVING_INPUT = 0x6130, RX_OT_LIVING_OUTPUT,
       RX_OT_LIVING_EVIDENCE, RX_OT_LIVING_CANDIDATE, RX_OT_LIVING_PROMOTION,
       RX_OT_LIVING_INFORCE, RX_OT_LIVING_RESTORE };

/* restore (R14): 0 boot sequence (outside: this body started), 1 boot taken
 * up, 2 outcome, 3 active generation read, 4 realization id word 0 restored,
 * 5 refusal reason, 6 lineage                                     (restore) */
enum { RX_LIVING_RESTORE_REFERENCE = 1, RX_LIVING_RESTORE_RESTORED,
       RX_LIVING_RESTORE_REFUSED };
/* Refusal reasons in restore field 5 besides an R9 or Omega code. */
enum { RX_LIVING_RESTORE_WHY_CONFIG = 1, RX_LIVING_RESTORE_WHY_IDENTITY };

typedef struct {
    RxObjRef input, output, evidence, candidate, promotion, inforce;
    RxObjRef restore;           /* only after rx_living_register_restore */
} RxLivingObjects;

/* Durable provenance written into the R9 candidate. Each link is the causal
 * crumb that last published that node and the crumb's digest. */
enum { RX_LINK_GOAL, RX_LINK_PLAN, RX_LINK_SEARCH, RX_LINK_GPU_INPUT, RX_LINK_GPU_OUTPUT,
       RX_LINK_EVIDENCE, RX_LINK_BELIEF, RX_LINK_SYNTH, RX_LINK_VERDICT, RX_LINK_MEASURE,
       RX_LINK_SELECTION, RX_LINK_COUNT };
typedef struct {
    RxObjRef obj;
    uint32_t reaction;          /* UINT32_MAX for an outside publication */
    uint64_t crumb;
    uint8_t digest[32];
} RxLivingLink;
typedef struct {
    char magic[8];              /* "R13PROV1" */
    uint64_t epoch, goal_seq, plan_seq;
    RxLivingLink link[RX_LINK_COUNT];
} RxLivingProvenance;

/* Configuration needed to rebuild the selected realization from its bytes. */
typedef struct {
    char magic[8];              /* "R13CONF1" */
    uint64_t regime, margin_pct, kind, code_len;
    uint8_t identity[32];
} RxLivingConfig;

typedef struct {
    char magic[8];              /* "R13EVID1" */
    uint64_t epoch, expected, observed;
    uint64_t selected_ps, reference_ps, trials;
} RxLivingEvidence;

typedef struct {
    /* living.experiment.prepare (RX_LIVING_SUBJ) */
    RxCapRef plan_read, search_read, input_write;
    /* living.blackwell.add (RX_LIVING_SEAT_SUBJ); output WRITE is in the slot */
    RxCapRef input_seat_read;
    RxObjRef output_slot;
    RxCapRef output_slot_read;
    /* living.experiment.evidence (RX_LIVING_SUBJ) */
    RxCapRef output_read, evidence_write;
    /* generation.prepare (RX_LIVING_PREPARE_SUBJ) */
    RxCapRef selection_read, belief_read, evidence_prepare_read, candidate_write;
    RxCapRef goal_prepare_read, plan_prepare_read, search_prepare_read, output_prepare_read;
} RxLivingCaps;

/* Kept apart from the proposer. Only generation.promote gets this context,
 * and only the native authority can validate its promotion right. */
typedef struct {
    RxWorld *world;
    RxGenStore *store;
    const struct AienosCapView *authority;
    RxObjRef candidate, promotion, inforce;
    RxCapRef candidate_read, promotion_write, inforce_write, promotion_authority;
    uint32_t reaction;
    int result;
    /* R16 C5: the promotion subject's credential (rx_caller.h), apart from
     * the proposer's; null in a world without bound callers. */
    const RxCallerKeyring *keys;
} RxLivingPromoter;

typedef struct {
    RxWorld *world;
    RxAienFaculty *aien;
    RxOmegaFaculty *omega;
    RxGenStore *store;
    const struct AienosCapView *authority;
    RxLivingObjects o;
    RxLivingCaps caps;
    uint32_t r_prepare, r_seat, r_evidence, r_candidate, r_restore;
    /* An R9 proposal is an effect outside the world. If the reaction that
     * made it is invalidated and runs again for the same epoch, it reuses
     * the proposal instead of making a second one. */
    uint64_t proposed_epoch, candidate_id;
    uint64_t refusals;          /* observability: why nothing was proposed */
    int last_refusal;
    /* R16 C5: credentials of the living subjects other than the promotion
     * subject (rx_caller.h); null in a world without bound callers. */
    const RxCallerKeyring *keys;
} RxLiving;

/* Why generation.prepare declined (last_refusal). Never a semantic decision
 * by the harness; these are the reaction's own checks. */
enum { RX_LIVING_WHY_AUTHOR = 1, RX_LIVING_WHY_STORE, RX_LIVING_WHY_CHAIN,
       RX_LIVING_WHY_GRANT, RX_LIVING_WHY_PROPOSE };

int rx_living_create(RxLiving *l, RxWorld *w, RxAienFaculty *aien,
                     RxOmegaFaculty *omega, RxGenStore *store,
                     const struct AienosCapView *authority);
int rx_living_register(RxLiving *l, const RxLivingCaps *caps,
                       RxLivingPromoter *promoter);

/* R14. generation.restore: the promotion subject rebuilds the in-force record
 * from R9 when the body starts. Woken by an outside publication of a new boot
 * sequence into restore field 0 (the body starting is an outside event). It
 * reads the active generation's realization, config and evidence through
 * rx_gen_read_blob, has Omega verify the bytes again (rx_omega_readmit), and
 * only then names them in force. Genesis leaves production on the reference;
 * anything that does not check out is refused and also leaves the reference.
 * `restore_write` is RW on RX_LIVING_RES_RESTORE for RX_LIVING_PROMOTE_SUBJ.
 * Creates the restore object. Call after rx_living_register. */
int rx_living_register_restore(RxLiving *l, RxLivingPromoter *promoter, RxCapRef restore_write);
/* R16 C6: the production subjects' runtime-issued caller credentials, one
 * keyring per component; each component holds only its own. */
typedef struct {
    RxCallerKeyring omega;      /* RX_OMEGA_SUBJ_SERVE, RX_OMEGA_SUBJ_OMEGA */
    RxCallerKeyring aien;       /* RX_AIEN_SUBJ */
    RxCallerKeyring aegis;      /* RX_AEGIS_SUBJ, RX_AEGIS_ROOT_SUBJ */
    RxCallerKeyring living;     /* RX_LIVING_SUBJ, _SEAT_SUBJ, _PREPARE_SUBJ */
    RxCallerKeyring promoter;   /* RX_LIVING_PROMOTE_SUBJ */
} RxLivingKeyrings;
/* Enrolls every production subject into w (rx_world_enroll_caller) and sets
 * one keyring per component (the faculties' .keys pointers are set by the
 * caller). Does not bind: the caller enrolls any further subjects, then calls rx_world_bind_callers. */
int rx_living_enroll_callers(RxWorld *w, RxLivingKeyrings *k);
/* The native promotion authority (aienos_cap_validate on an AienosCapView as
 * ctx), for rx_gen_bind_authority. */
int rx_living_native_authority(void *ctx, uint32_t cap_id, uint64_t generation,
                               uint32_t subject, uint64_t resource, uint32_t rights);

/* Same content digest the world gives an object: SHA-256 over
 * "AIEN_RX_OBJECT_V1", type and the eight fields. */
void rx_living_object_digest(uint32_t type, const uint64_t field[RX_MAX_FIELDS],
                             uint8_t out[32]);

#endif
