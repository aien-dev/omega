/*
 * rx_route.h -- Omega cognitive compute routing (OMEGA_COGNITIVE_ROUTING).
 *
 * A request states what cognition it needs (CognitiveRequirement). Several
 * engines may be able to supply it (CognitiveRealization). The router picks
 * the cheapest escalation ladder that the evidence says will meet the
 * requirement, runs the cheapest rung, and climbs only when the answer's
 * verification or calibrated confidence is not enough.
 *
 * Nothing here assumes an engine is a language model. An engine is a
 * function with a declared class, the operations it supports, its hard
 * capacities, and a measured profile.
 *
 * The measured profiles (quality, uncertainty calibration, cost, latency,
 * energy, evidence) are a generation's "model" blob. The router reads them
 * only from the active generation of an RxGenStore, through the SHA-256 root
 * check. It has no call that installs profiles from memory. Observations go
 * into a ledger, the ledger becomes a candidate generation, and the routing
 * changes only after that candidate is promoted with the promotion right on
 * the native authority. rx_route.o links no rx_gen_promote and no aienos_cap_*
 * symbol; the build checks this.
 */
#ifndef RX_ROUTE_H
#define RX_ROUTE_H

#include "rx_generation.h"

#include <stddef.h>
#include <stdint.h>

#define RX_COG_OK                 0
#define RX_COG_ERR_ARG          -60
#define RX_COG_ERR_UNSATISFIABLE -61 /* no ladder is expected to meet the requirement */
#define RX_COG_ERR_EXHAUSTED    -62  /* the ladder ran out before an answer was accepted */
#define RX_COG_ERR_MODEL        -63  /* the generation's model is missing, torn or foreign */
#define RX_COG_ERR_FULL         -64
#define RX_COG_ERR_ENGINE       -65

#define RX_COG_MAX_ENGINES   8u
#define RX_COG_MAX_OPS       4u
#define RX_COG_MAX_LADDER    4u    /* rungs in one ladder: at most 3 escalations */
#define RX_COG_BINS          16u   /* confidence bins */
#define RX_COG_MAX_SAMPLE    2048u /* evidence records kept per operation */
#define RX_COG_MIN_EVIDENCE  200u  /* fewer records: the profile does not qualify anything */
#define RX_COG_CONF_ONE      1000000u

/* Operation classes. */
enum { RX_COG_OP_RECOGNIZE = 0, /* label an observation; no cheap verifier */
       RX_COG_OP_PLAN = 1,      /* reach a goal within a bound; answers are checkable */
       RX_COG_OP_COUNT = 2 };

/* Realization classes. */
enum { RX_COG_CLASS_DETERMINISTIC = 1, /* compiled procedure */
       RX_COG_CLASS_SMALL_POLICY,      /* small learned policy */
       RX_COG_CLASS_NEURAL_MODULE,     /* specialized neural module */
       RX_COG_CLASS_GENERAL,           /* general cognition */
       RX_COG_CLASS_ENSEMBLE,
       RX_COG_CLASS_JSPACE };          /* exhaustive exploration of the answer space */

/* Hardware a realization needs / a requirement allows. */
enum { RX_COG_HW_CPU_P = 1u, RX_COG_HW_CPU_E = 2u, RX_COG_HW_GPU = 4u };

enum { RX_COG_PREC_APPROX = 1, RX_COG_PREC_EXACT = 2 };

typedef struct {
    uint32_t operation_class;
    uint32_t minimum_quality_ppm;    /* fraction of answers that must be right, over the class */
    uint32_t uncertainty_budget_ppm; /* an accepted answer's calibrated error may not exceed this */
    uint64_t memory_requirement;     /* bytes of memory the cognition must hold */
    uint32_t temporal_requirement;   /* steps of history the answer depends on (0 = none) */
    uint32_t planning_depth;         /* longest plan the answer may need */
    uint32_t precision_requirement;  /* RX_COG_PREC_* */
    uint64_t latency_budget_ns;      /* expected time of the whole ladder must fit (admission) */
    uint64_t energy_budget_nj;       /* expected energy for the whole ladder (0 = unbounded) */
    uint32_t hardware_constraints;   /* RX_COG_HW_* the request may use */
    uint32_t escalation_allowed;     /* escalations allowed after the first rung (0 = none) */
} RxCogRequirement;

/* What an engine declares about itself. Fixed at registration; part of the
 * registry digest a model is bound to. */
typedef struct {
    uint32_t cognitive_engine_id;
    uint32_t realization_class;
    uint32_t supported_operations;   /* bit per RX_COG_OP_* */
    uint32_t hardware_requirements;  /* all of these must be allowed */
    uint64_t memory_capacity;
    uint32_t temporal_horizon;
    uint32_t max_planning_depth;
    uint32_t precision;              /* RX_COG_PREC_* */
    uint32_t verifiable;             /* answers carry a sound check (confidence 0 or 1) */
} RxCogDeclaration;

typedef struct {
    uint32_t n[RX_COG_BINS];        /* observations per confidence bin */
    uint32_t correct[RX_COG_BINS];
} RxCogCalibration;

typedef struct {
    uint32_t observations;
    uint32_t correct;
    uint32_t failures;              /* engine errors */
} RxCogQuality;

typedef struct {
    uint64_t calls;
    uint64_t mean_ns;               /* time per call on the calibrating core */
    uint64_t p99_ns;
    uint64_t nj_per_call;           /* energy per call, from the energy meter; 0 = not measured */
    uint32_t energy_measured;
} RxCogCost;

/* The routing view of one engine: declaration plus the profile of the
 * active generation. `generation` is the id of the generation the profile
 * came from (0 = none loaded). */
typedef struct {
    uint32_t cognitive_engine_id;
    uint64_t generation;
    RxCogDeclaration decl;
    RxCogQuality quality_profile[RX_COG_OP_COUNT];
    RxCogCalibration uncertainty_profile[RX_COG_OP_COUNT];
    RxCogCost resource_cost_model[RX_COG_OP_COUNT]; /* latency_model = mean/p99, energy_model = nj */
    uint8_t evidence[32];           /* SHA-256 of the evidence sample this profile came from */
} RxCogRealization;

/* An engine answers `input` into `output`. confidence is 0..RX_COG_CONF_ONE.
 * A verifiable engine reports 0 or RX_COG_CONF_ONE and only after its check.
 * Returns 0, or nonzero on an engine failure. */
typedef int (*RxCogEngineFn)(void *ctx, uint32_t op, const void *input, void *output,
                             uint32_t *confidence);

typedef struct RxCogRouter RxCogRouter;

/* One ladder: engines in the order they are tried. */
typedef struct {
    uint32_t n;
    uint32_t engine[RX_COG_MAX_LADDER];
    uint32_t accept_min_bin[RX_COG_MAX_LADDER]; /* accept a rung's answer from this bin up */
    uint64_t expected_nj;           /* 0 when energy is not measured */
    uint64_t expected_ns;
    uint32_t expected_quality_ppm;  /* on the evidence sample */
    uint32_t quality_lower_ppm;     /* Wilson 95% lower bound */
    uint32_t reason;                /* RX_COG_WHY_* */
} RxCogLadder;

enum { RX_COG_WHY_EVIDENCE = 1,     /* cheapest ladder the evidence supports */
       RX_COG_WHY_NO_EVIDENCE };    /* no profile yet: the reference realization alone */

typedef struct {
    uint32_t rungs_run;
    uint32_t engine[RX_COG_MAX_LADDER];
    uint32_t confidence[RX_COG_MAX_LADDER];
    uint64_t ns[RX_COG_MAX_LADDER];
    uint32_t final_engine;
    uint32_t escalations;
    int accepted;                   /* 1 accepted, 0 exhausted */
} RxCogTrace;

int rx_route_create(RxCogRouter **out);
void rx_route_destroy(RxCogRouter *r);

/* Register before the first load. `reference` marks the realization used for
 * an operation while no evidence exists (at most one per operation). */
int rx_route_register(RxCogRouter *r, const RxCogDeclaration *decl, RxCogEngineFn fn, void *ctx,
                      uint32_t reference_ops);

/* SHA-256 of all declarations; a model is bound to it. */
void rx_route_registry_digest(const RxCogRouter *r, uint8_t out[32]);

/* Load the profiles of the active generation. With no committed generation,
 * or a generation whose model is empty, the router has no profiles and uses
 * each operation's reference realization. A torn or foreign model refuses
 * and leaves the loaded profiles as they were. */
int rx_route_load(RxCogRouter *r, const RxGenStore *store);
uint64_t rx_route_generation(const RxCogRouter *r);

int rx_route_realization(const RxCogRouter *r, uint32_t engine_id, RxCogRealization *out);

/* Choose the ladder for a requirement (cached per requirement and
 * generation). */
int rx_route_plan(RxCogRouter *r, const RxCogRequirement *req, RxCogLadder *out);

/* Choose, then run the ladder. */
int rx_route_run(RxCogRouter *r, const RxCogRequirement *req, const void *input, void *output,
                 size_t output_size, RxCogTrace *trace);

/* The always-most-expensive policy used as the gate baseline: the eligible
 * realization with the highest measured cost, alone. */
int rx_route_most_expensive(RxCogRouter *r, const RxCogRequirement *req, uint32_t *engine_id);

/* ---- learning ---- */

typedef struct RxCogLedger RxCogLedger;

int rx_route_ledger_create(const RxCogRouter *r, RxCogLedger **out);
void rx_route_ledger_destroy(RxCogLedger *l);

/* One labelled trial: every listed engine ran on the same request.
 * conf/correct/ns/failed are indexed like engines[]. */
int rx_route_observe(RxCogLedger *l, uint32_t op, uint32_t n, const uint32_t *engines,
                     const uint32_t *conf, const uint8_t *correct, const uint64_t *ns,
                     const uint8_t *failed);
/* Batch cost of `calls` calls of one engine on one operation: processor time,
 * and metered energy when energy_measured. The mean cost in the model comes
 * from batches when there are any (no clock read per call). */
int rx_route_observe_batch(RxCogLedger *l, uint32_t engine_id, uint32_t op, uint64_t calls,
                           uint64_t cpu_ns, int energy_measured, uint64_t nj);

/* Serialize the ledger into a model blob (caller frees *out). */
int rx_route_ledger_model(const RxCogLedger *l, uint8_t **out, size_t *out_len);

/* Propose the ledger as a candidate generation. It is not active, and the
 * router does not change, until someone holding the promotion right promotes
 * it. */
int rx_route_propose(const RxCogLedger *l, RxGenStore *store, uint32_t proposer,
                     uint64_t authority_epoch, uint64_t authority_generation, uint64_t *candidate);

#endif
