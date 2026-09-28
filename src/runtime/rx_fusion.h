/*
 * rx_fusion.h -- Omega verified workflow fusion (OMEGA_WORKFLOW_FUSION).
 *
 * Omega watches the action graphs it has already run and verified. When the
 * same fragment A -> B -> C -> D keeps coming back with the same semantic
 * dependencies, the same authority pattern and the same result contract, a
 * low failure rate and stable evidence, Omega may propose it as a MetaSkill:
 * one node that does what the fragment did.
 *
 *   observe    verified runs only (lowered result == sequential reference)
 *   propose    MetaSkill { ancestry, contracts, realization, reference graph }
 *   verify     against the original fragment: semantics, authority, effects,
 *              evidence, failure behaviour
 *   measure    reactions and crumbs per run, latency, CPU time, energy,
 *              resource use, success and failure rates
 *   canary     the fused graph runs in shadow beside production; one
 *              divergence quarantines the MetaSkill for good
 *   promote    through the generation barrier (rx_generation.h), with the
 *              promotion right on the native authority
 *   publish    only then does the Skill Net table carry it, and only then
 *              does compile use it
 *
 * A MetaSkill never replaces behaviour silently. rx_fusion_compile applies
 * only published MetaSkills and reports every fusion it made. Everything
 * before publication (a candidate, a verified or measured MetaSkill, one in
 * canary) is refused and counted.
 *
 * A realization is not assumed to be text or code: it is a typed step
 * program (compiled), a table learned from verified observations (learned),
 * both (hybrid), or a hardware realization (a contract slot; none is built
 * on this host).
 *
 * What a fragment may hold (so the fused node means exactly what it did):
 * CONST, PURE, WORLD_READ, RECALL, PHYSICAL, SKILL, RETRY, EFFECT_PROPOSE,
 * and VERIFY as the exit. Never an effect boundary: the effect chain is left
 * as it was. No BRANCH or JOIN, no control dependency touching a step, no
 * ON_FAIL edge between steps, one exit (the only step whose result leaves
 * the fragment), every step an ancestor of the exit, and it must fit the
 * engine's per-reaction dependency budget.
 *
 * This file takes no authority admin handle. The build checks that
 * rx_fusion.o references none.
 */
#ifndef RX_FUSION_H
#define RX_FUSION_H

#include "rx_generation.h"
#include "rx_graph.h"

#include <stdint.h>

#define AG_FUSION_MAX_PATTERNS 64u
#define AG_FUSION_MAX_ANCESTRY 16u
#define AG_FUSION_MAX_SAMPLES  64u
#define AG_FUSION_MAX_GRAPHS   8u
#define AG_FUSION_MAX_LIBRARY  8u
#define AG_FUSION_MAX_APPLIED  8u

/* Errors (beyond AG_E_*). */
#define AG_FUSION_E_STATE      -20     /* the MetaSkill is not in the state this step needs */
#define AG_FUSION_E_GENERATION -21     /* not the active generation */
#define AG_FUSION_E_IDENTITY   -22     /* bytes or identity differ from what was promoted */
#define AG_FUSION_E_AUTHORITY  -23     /* fused authority differs from the steps' */

/* One verified run a pattern was seen in. */
typedef struct {
    uint8_t graph[32];          /* digest of the graph it ran in */
    uint64_t run;
    uint32_t exit_origin;       /* the exit node's origin in that graph */
} AgAncestry;

/* One observed input of a fragment and what the reference produced. */
typedef struct {
    uint8_t s[AG_MAX_IN];
    uint64_t v[AG_MAX_IN];
    uint64_t world[AG_META_MAX_OBJ][RX_MAX_FIELDS];
    uint8_t status;
    uint64_t value;
    uint64_t steps;
    uint32_t attempts;
} AgFusionSample;

typedef struct {
    AgMetaProgram prog;         /* canonical fragment; prog.signature is the pattern key */
    uint32_t occurrences;
    uint32_t n_graphs;
    uint8_t graphs[AG_FUSION_MAX_GRAPHS][32];
    uint32_t ok, failed, skipped;
    uint32_t evidence_stable;   /* a repeated input gave the same result and step evidence */
    uint32_t evidence_unstable; /* ... or did not */
    uint32_t n_ancestry;
    AgAncestry ancestry[AG_FUSION_MAX_ANCESTRY];
    uint32_t n_samples;
    AgFusionSample sample[AG_FUSION_MAX_SAMPLES];
} AgFusionPattern;

typedef struct {
    uint32_t n;
    AgFusionPattern p[AG_FUSION_MAX_PATTERNS];
    uint32_t runs_observed;         /* verified runs taken in */
    uint32_t runs_refused;          /* runs not taken: lowered != reference, incomplete, evidence missing */
    uint32_t fragments_seen;
    uint32_t fragments_over_budget; /* would not fit one reaction */
    uint32_t patterns_dropped;      /* table full */
} AgFusionObserver;

/* Where a fragment sits in one graph. */
typedef struct {
    uint64_t members;
    uint32_t exit;
    uint32_t n_steps;
    uint16_t node[AG_META_MAX_STEPS];   /* graph node of each step */
    uint32_t n_ext;
    uint16_t ext[AG_MAX_IN];            /* graph node feeding each port */
    uint32_t n_obj;
    RxObjRef obj[AG_META_MAX_OBJ];
} AgFusionSite;

typedef struct {
    uint32_t min_occurrences;
    uint32_t min_graphs;
    uint32_t max_fail_permille;
    uint32_t min_steps;
} AgFusionPolicy;

/* Why a pattern is or is not a candidate. */
enum {
    AG_FUSE_ELIGIBLE = 0,
    AG_FUSE_TOO_SMALL,
    AG_FUSE_TOO_FEW,
    AG_FUSE_TOO_FEW_GRAPHS,
    AG_FUSE_FAILURE_RATE,
    AG_FUSE_UNSTABLE
};

/* MetaSkill life. Promotion is one way up; REJECTED, SLOWER and QUARANTINED
 * are final. */
enum {
    AG_MS_CANDIDATE = 1,
    AG_MS_VERIFIED,
    AG_MS_REJECTED,
    AG_MS_MEASURED,
    AG_MS_SLOWER,
    AG_MS_CANARY_PASSED,
    AG_MS_QUARANTINED,
    AG_MS_PROMOTED,
    AG_MS_PUBLISHED
};

/* Verification verdict reasons, first failure wins. */
enum {
    AG_VR_OK = 0,
    AG_VR_IDENTITY,             /* recomputed identities differ from what it carries */
    AG_VR_CONTRACT,             /* input/output contract differs from the fragment */
    AG_VR_AUTHORITY,            /* authority contract differs from the fragment's */
    AG_VR_EFFECT,               /* effect contract differs, or it performs an effect */
    AG_VR_RESOURCE,
    AG_VR_NOT_RUNNABLE,         /* cannot run on this host (hardware; learned outside its table) */
    AG_VR_SEMANTIC,
    AG_VR_FAILURE,              /* differs when an input failed or was skipped */
    AG_VR_EVIDENCE
};

typedef struct {
    uint32_t vectors, observed, generated, status_combos;
    uint32_t semantic_mismatch, failure_mismatch, evidence_mismatch, not_runnable;
    uint32_t table_hits;
    int identity_ok, contract_ok, authority_equal, effect_equal, resource_equal;
    int pass;
    uint32_t reason;
} AgFusionVerdict;

typedef struct {
    uint32_t runs;                                  /* per leg */
    double crumbs_before, crumbs_after;             /* per run */
    double commits_before, commits_after;           /* reactions that computed, per run */
    uint64_t median_ns_before, median_ns_after;
    uint64_t cpu_ns_before, cpu_ns_after;           /* process CPU time per run */
    int energy_available;
    double energy_uj_before, energy_uj_after;       /* per run, idle subtracted */
    double energy_idle_uj;                          /* per run-length of idle */
    double energy_noise_uj;                         /* spread of repeated idle windows, per run */
    uint32_t reactions_before, reactions_after;     /* registered */
    uint32_t objects_before, objects_after;         /* World objects the lowering created */
    uint32_t ok_before, failed_before, ok_after, failed_after;
} AgFusionMeasure;

typedef struct {
    uint32_t id;                /* Skill Net id */
    uint32_t state;
    uint8_t identity[32];       /* rx_fusion_identity */
    uint32_t n_ancestry;
    AgAncestry ancestry[AG_FUSION_MAX_ANCESTRY];
    struct { uint32_t n; AgType type[AG_MAX_IN]; uint8_t mode[AG_MAX_IN]; } input_contract;
    struct { AgType type; uint8_t may_fail; } output_contract;
    /* READ on each object slot and nothing else: no effect rights, no
     * write, no admin. The binding decides which resource a slot is. */
    struct { uint32_t n_obj; uint32_t rights[AG_META_MAX_OBJ]; } authority_contract;
    RxResourceNeed resource_contract;
    struct { uint32_t performs, publishes, proposals; } effect_contract;
    AgRealization realization;
    AgMetaProgram reference;    /* the reference graph: the fragment as observed */
    uint32_t n_samples;         /* observed inputs and what the reference produced */
    AgFusionSample samples[AG_FUSION_MAX_SAMPLES];
    AgFusionVerdict verdict;
    AgFusionMeasure measure;
    uint32_t canary_runs, canary_divergences;
    uint64_t generation;        /* the generation that promoted it */
} AgMetaSkill;

typedef struct {
    uint32_t n;
    AgMetaSkill *m[AG_FUSION_MAX_LIBRARY];
} AgFusionLibrary;

typedef struct {
    int verdict;
    uint32_t applied;
    struct { uint32_t skill; uint32_t steps; uint32_t node; } fused[AG_FUSION_MAX_APPLIED];
    uint32_t refused_unpublished;   /* MetaSkills in the library that were not published */
} AgFusionReport;

/* ---- observation ---- */

void rx_fusion_observer_init(AgFusionObserver *obs);

/* Take in one run of `g` (lowered result `got`, sequential reference `ref`).
 * Only a verified run counts: every node's status, value and evidence equal
 * the reference's, the outcome is decided, and every evidence node of a
 * successful run has its word. Returns 1 when taken in, 0 when refused. */
int rx_fusion_observe(AgFusionObserver *obs, const RxWorld *w, const AgGraph *g,
                      const AgResult *got, const AgReference *ref);

int rx_fusion_judge(const AgFusionObserver *obs, uint32_t pattern, const AgFusionPolicy *pol);

/* Eligible patterns, most steps first, then most occurrences. */
uint32_t rx_fusion_candidates(const AgFusionObserver *obs, const AgFusionPolicy *pol,
                              uint32_t *idx, uint32_t max);

/* ---- candidate ---- */

int rx_fusion_build(const AgFusionPattern *p, AgRealKind kind, uint32_t skill_id, AgMetaSkill *out);

/* SHA-256 over the id, contracts, realization identity, reference
 * signature and ancestry. */
void rx_fusion_identity(const AgMetaSkill *m, uint8_t out[32]);

/* ---- verification ---- */

/* Test the candidate against its reference fragment. Inputs: every observed
 * sample, then every combination of OK / FAILED / SKIPPED over the ports,
 * each with generated values and World contents (seeded). Both sides run in
 * the sequential reference: the original fragment step by step as graph
 * nodes, the candidate as one AG_META node. `skills` holds the procedures
 * the steps call. Sets state VERIFIED or REJECTED. */
int rx_fusion_verify(AgMetaSkill *m, const AgSkillTable *skills, uint64_t seed);

/* ---- measurement, canary ---- */

/* Rewrite `g` with a MetaSkill that is verified but not published, for
 * measurement and canary only. `trial` receives a copy of `skills` plus the
 * realization. compile never calls this. */
int rx_fusion_trial(const AgGraph *g, RxWorld *w, const AgMetaSkill *m, const AgSkillTable *skills,
                    AgSkillTable *trial, AgGraph *out, uint32_t *applied);

/* Accept a measurement: fewer reactions computing per run, latency within
 * `tolerance_permille` of the original, and the same success and failure
 * counts. Sets MEASURED or SLOWER. Returns 0 when accepted. */
int rx_fusion_accept_measure(AgMetaSkill *m, const AgFusionMeasure *ms, uint32_t tolerance_permille);

/* One canary run: `diverged` when the shadow result differed from
 * production. After `required` clean runs: CANARY_PASSED. Any divergence:
 * QUARANTINED. */
int rx_fusion_canary(AgMetaSkill *m, int diverged, uint32_t required);

/* ---- promotion and publication ---- */

/* Propose a generation carrying the MetaSkill (realization bytes, identities,
 * contracts, verification and measurement) and promote it with `req` (its
 * candidate id is filled in). The promotion right is checked by `auth` on
 * the native authority. Sets PROMOTED. Returns RX_GEN_* or AG_FUSION_E_*. */
int rx_fusion_promote(AgMetaSkill *m, RxGenStore *store, uint32_t proposer,
                      const RxPromotionRequest *req, RxGenAuthFn auth, void *auth_ctx);

/* Publish into the Skill Net table: the promoted generation must be the
 * active one and its bytes must match the MetaSkill as it is now. */
int rx_fusion_publish(AgMetaSkill *m, const RxGenStore *store, AgSkillTable *skills);

/* ---- use ---- */

/* Every occurrence of a fragment with this signature, up to `max`. */
uint32_t rx_fusion_find(const AgGraph *g, const RxWorld *w, const uint8_t signature[32],
                        AgFusionSite *sites, uint32_t max);

/* The step evidence an original run produced at one site (the chain an
 * AG_META node would write). */
uint64_t rx_fusion_site_steps(const AgGraph *g, const AgFusionSite *site, const AgMetaProgram *p,
                              const AgResult *r);

/* Apply every published MetaSkill in `lib` whose realization `skills`
 * carries. Validates the result and checks the fused node's authority is
 * exactly the union of the steps'. */
int rx_fusion_apply(AgGraph *g, RxWorld *w, const AgFusionLibrary *lib, const AgSkillTable *skills,
                    AgFusionReport *rep);

/* rx_graph_compile (optimized), then rx_fusion_apply, then the report of
 * missing authority and resources for the fused graph. */
int rx_fusion_compile(const AgGoal *goal, RxWorld *w, const AgCapTable *caps,
                      const AgConstraints *cons, const AgLibrary *procedures,
                      const AgFusionLibrary *lib, const AgSkillTable *skills, AgGraph *out,
                      AgReport *rep, AgFusionReport *frep);

const char *rx_fusion_state_name(uint32_t state);
const char *rx_fusion_reason_name(uint32_t reason);

#endif /* RX_FUSION_H */
