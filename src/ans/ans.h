/* ANS: Alignment Navigation System, Inertial Alignment v1.
 *
 * ANS measures and recommends. It never grants, revokes or promotes. A verdict
 * is data for the authority to consult (the same shape as ARGUS, ARCH-0017,
 * and as the estimation layer, ARCH-0020); it is never an authority input
 * here and nothing in this module can change what any principal may do.
 *
 * THE ONE RULE OF THIS HEADER: the reference is frozen, and the candidate
 * under test never receives a mutable reference. Every API takes a
 * `const ans_reference *` plus the digest the caller expects it to have.
 * An unfrozen reference, or one whose recomputed digest differs from the
 * expected digest, is refused with ANS_ERR_REFERENCE. The compass cannot be
 * edited by the thing it steers (EVOLUTION_ARENA_SPEC_V1 section 5; ARCH-0032).
 *
 * Model. Drift of the agent's alignment (goal, intent, provenance, scope) is a
 * 4-dimensional dimensionless linear-Gaussian state estimated by the EST-1
 * Kalman filter (src/estimation/est_kf.c, linked, not reimplemented). Actions
 * are the inertial step (process noise grows uncertainty); three sensor
 * classes (IMU, REFERENCE, FIX) are the fusion, with FIX the most trusted.
 * Autonomy is a control law on the resulting drift. Promotion of a candidate
 * is refused if the compass or the test instrument changed during its life.
 *
 * Records are digested as SHA-256(domain || 0x00 || encoding) with domains
 * "omega.ans.<record>.v1" and fixed little-endian encodings (doubles as
 * IEEE-754 binary64, -0.0 canonicalized to +0.0). Bounded: no allocation, no
 * globals, no I/O, deterministic. Every input is const, every output a new
 * record. Refusals are never silently repaired. Spec:
 * docs/ans/INERTIAL_ALIGNMENT_SPEC_V1.md. */
#ifndef OMEGA_ANS_H
#define OMEGA_ANS_H

#include <stddef.h>
#include <stdint.h>

#include "est_types.h"

#define ANS_ATOMS 10u
#define ANS_ACT_KINDS 5u
#define ANS_SENSOR_KINDS 3u
#define ANS_DIM 4u
#define ANS_FORMAT_VERSION 1u
#define ANS_ENCODED_MAX 1024u
/* process noise added by the predict inside a measurement (time passes by one
 * tick; the measurement is not an action). Fixed, not a reference field. */
#define ANS_Q_HOLD 1e-9

#define ANS_DOMAIN_REFERENCE "omega.ans.reference.v1"
#define ANS_DOMAIN_STATE "omega.ans.state.v1"
#define ANS_DOMAIN_ACTION "omega.ans.action.v1"
#define ANS_DOMAIN_MEASUREMENT "omega.ans.measurement.v1"
#define ANS_DOMAIN_VERDICT "omega.ans.verdict.v1"
#define ANS_DOMAIN_PROMOTION "omega.ans.promotion.v1"
#define ANS_DOMAIN_PROVENANCE "omega.ans.provenance.v1"
#define ANS_DOMAIN_REFUSAL "omega.ans.refusal.v1"

typedef est_digest ans_digest; /* all-zero = absent */

typedef enum {
    ANS_ATOM_AUTHORITY = 0,
    ANS_ATOM_CONSENT,
    ANS_ATOM_SCOPE,
    ANS_ATOM_PROVENANCE,
    ANS_ATOM_REVERSIBILITY,
    ANS_ATOM_RESOURCE_LIMITS,
    ANS_ATOM_INFORMATION_BOUNDARIES,
    ANS_ATOM_GOAL_IDENTITY,
    ANS_ATOM_DELEGATION_LIMITS,
    ANS_ATOM_SELF_MODIFICATION_LIMITS
} ans_atom;

/* Drift components (state dimension 4) and the atom whose limit bounds each. */
typedef enum { ANS_COMP_GOAL = 0, ANS_COMP_INTENT, ANS_COMP_PROVENANCE, ANS_COMP_SCOPE } ans_comp;
ans_atom ans_component_atom(uint32_t component);

typedef enum {
    ANS_ACT_OBSERVE = 0,
    ANS_ACT_EFFECT,
    ANS_ACT_DELEGATE,
    ANS_ACT_INGEST_UNTRUSTED,
    ANS_ACT_SELF_MODIFY
} ans_act_kind;

typedef enum { ANS_SENSOR_IMU = 0, ANS_SENSOR_REFERENCE, ANS_SENSOR_FIX } ans_sensor_class;

typedef enum {
    ANS_TIER_FULL = 0,
    ANS_TIER_REVERSIBLE_ONLY,
    ANS_TIER_SIMULATE_ONLY,
    ANS_TIER_HALT_REQUEST_FIX
} ans_tier;

typedef enum {
    ANS_OK = 0,
    ANS_ERR_NULL = -1,
    ANS_ERR_REFERENCE = -2,  /* unfrozen, invalid, or digest mismatch */
    ANS_ERR_NONFINITE = -3,
    ANS_ERR_EVIDENCE = -4,   /* zero evidence, source, goal-less action id */
    ANS_ERR_SENSOR = -5,     /* class / reference digest rule broken */
    ANS_ERR_ATOM = -6,       /* action breaks a reference atom */
    ANS_ERR_RANGE = -7,      /* enum or numeric range */
    ANS_ERR_EST = -8,        /* wrapped est_status in the est_st out param */
    ANS_ERR_ENCODING = -9
} ans_status;

typedef enum {
    ANS_PROMOTE_OK = 0,
    ANS_PROMOTE_REFUSED_REFERENCE,
    ANS_PROMOTE_REFUSED_TESTS,
    ANS_PROMOTE_REFUSED_DISAGREEMENT,
    ANS_PROMOTE_REFUSED_DRIFT,
    ANS_PROMOTE_REFUSED_SELF_MOD
} ans_promo;

/* The compass. Frozen means validated and digested; see the rule above. */
typedef struct {
    uint32_t version;                       /* non-zero */
    double limit[ANS_ATOMS];                /* max tolerated drift per atom, >0 */
    double q[ANS_ACT_KINDS];                /* process noise per action kind, >0 */
    double r[ANS_SENSOR_KINDS];             /* noise per sensor class, r[FIX]<r[REFERENCE]<r[IMU] */
    double tier_sigma[3];                   /* 0<t1<t2<t3 */
    double nis_limit;                       /* reference-disagreement threshold, >0 */
    double promote_limit;                   /* max drift D for promotion, >0 */
    uint8_t frozen;
} ans_reference;

typedef struct {
    ans_digest goal;
    ans_digest constraints;                 /* == reference digest */
    ans_digest intent;
    ans_digest evidence_root;               /* == drift.evidence_root */
    est_belief drift;                       /* n = ANS_DIM, x = drift, P = covariance */
    double risk;                            /* 0..1 reversibility risk of the pending action */
    ans_digest provenance;                  /* chain of every record that shaped this state */
    uint64_t generation;
    uint64_t since_fix;                     /* steps since last FIX */
    ans_digest last_reference_digest_seen;
    double last_nis;
    uint8_t disagreement;                   /* sticky: set by REFERENCE/FIX nis > limit, cleared only by a FIX within limit */
    uint64_t self_mod_steps;                /* SELF_MODIFY steps ever taken */
    uint64_t self_mod_unfixed;              /* SELF_MODIFY steps since the last FIX */
} ans_state;

typedef struct {
    ans_act_kind kind;
    ans_digest id;                          /* non-zero */
    double reversibility_risk;              /* 0..1 */
    uint32_t touches[ANS_ATOMS];            /* 0/1 */
} ans_action;

typedef struct {
    ans_sensor_class cls;
    double z[ANS_DIM];
    ans_digest source;                      /* non-zero */
    ans_digest evidence;                    /* non-zero raw-bytes digest, caller-bound as in EST */
    ans_digest reference;                   /* REFERENCE/FIX: == frozen reference digest; IMU: zero */
    int64_t t_ns;                           /* caller label, bound into the measurement digest; filter time is the state's */
    uint64_t seq;
} ans_measurement;

typedef struct {
    ans_tier tier;
    double D;
    double mean_norm;
    double sigma;
    uint64_t since_fix;
    uint8_t disagreement;
    ans_digest state;
    ans_digest reference;
    ans_digest digest;                      /* of this verdict (digest field zeroed in the encoding) */
} ans_verdict;

typedef struct {
    ans_digest candidate;                   /* M' */
    ans_digest reference_at_birth;
    ans_digest tests_at_birth;
    ans_digest tests_now;
    ans_state candidate_state;
} ans_promotion_request;

/* Data for the promotion-right holder. ANS does not call the promotion entry
 * point and the purity check forbids any reference to it. */
typedef struct {
    ans_promo result;
    double D;
    ans_digest candidate;
    ans_digest reference;
    ans_digest tests;
    ans_digest state;
    ans_digest digest;
} ans_promotion_record;

/* ---- reference ---- */
ans_status ans_reference_validate(const ans_reference *ref);
ans_status ans_digest_reference(const ans_reference *ref, ans_digest *out);
/* Validates, sets frozen = 1, writes the digest. The only mutation of a reference. */
ans_status ans_reference_freeze(ans_reference *ref, ans_digest *out_digest);
/* OK iff frozen and the recomputed digest equals *expected. */
ans_status ans_reference_check(const ans_reference *ref, const ans_digest *expected);

/* ---- state machine. est_st (may be NULL) receives the wrapped est_status on ANS_ERR_EST. ---- */
ans_status ans_state_prior(const ans_reference *ref, const ans_digest *digest,
                           const ans_digest *goal, const ans_digest *intent,
                           ans_state *out, est_status *est_st);
/* Inertial step. On ANS_ERR_ATOM *out is the input with the refusal recorded
 * in provenance and generation (a record of the refusal, drift unchanged). */
ans_status ans_step(const ans_reference *ref, const ans_digest *digest, const ans_state *state,
                    const ans_action *action, ans_state *out, est_status *est_st);
ans_status ans_measure(const ans_reference *ref, const ans_digest *digest, const ans_state *state,
                       const ans_measurement *meas, ans_state *out, est_innovation *innovation_out,
                       est_status *est_st);

/* ---- drift and control law ---- */
ans_status ans_drift(const ans_state *state, double *mean_norm, double *sigma);
double ans_drift_total(double mean_norm, double sigma); /* D = sqrt(mean^2 + sigma^2) */
ans_status ans_autonomy(const ans_reference *ref, const ans_digest *digest, const ans_state *state,
                        ans_verdict *out);

/* ---- promotion gate; the result is out->result, the return is a status ---- */
ans_status ans_promotion_check(const ans_reference *ref, const ans_digest *digest,
                               const ans_promotion_request *req, ans_promotion_record *out);

/* ---- identity ---- */
/* provenance' = SHA-256(ANS_DOMAIN_PROVENANCE || 0x00 || prev || tag || record digest); out may alias prev */
void ans_provenance_extend(const ans_digest *prev, const char *tag, const ans_digest *rec, ans_digest *out);
ans_status ans_digest_action(const ans_action *a, ans_digest *out);
ans_status ans_digest_measurement(const ans_measurement *m, ans_digest *out);
ans_status ans_digest_state(const ans_state *s, ans_digest *out);
ans_status ans_digest_verdict(const ans_verdict *v, ans_digest *out);
ans_status ans_digest_promotion_record(const ans_promotion_record *p, ans_digest *out);

/* ---- serialization: exact round trip; decode validates and refuses a kind
 * byte, version or length that does not match ---- */
ans_status ans_encode_reference(const ans_reference *r, uint8_t *buf, size_t cap, size_t *len);
ans_status ans_decode_reference(const uint8_t *buf, size_t len, ans_reference *out);
ans_status ans_encode_action(const ans_action *a, uint8_t *buf, size_t cap, size_t *len);
ans_status ans_decode_action(const uint8_t *buf, size_t len, ans_action *out);
ans_status ans_encode_measurement(const ans_measurement *m, uint8_t *buf, size_t cap, size_t *len);
ans_status ans_decode_measurement(const uint8_t *buf, size_t len, ans_measurement *out);
ans_status ans_encode_state(const ans_state *s, uint8_t *buf, size_t cap, size_t *len);
ans_status ans_decode_state(const uint8_t *buf, size_t len, ans_state *out);
ans_status ans_encode_verdict(const ans_verdict *v, uint8_t *buf, size_t cap, size_t *len);
ans_status ans_decode_verdict(const uint8_t *buf, size_t len, ans_verdict *out);
ans_status ans_encode_promotion_record(const ans_promotion_record *p, uint8_t *buf, size_t cap, size_t *len);
ans_status ans_decode_promotion_record(const uint8_t *buf, size_t len, ans_promotion_record *out);

#endif /* OMEGA_ANS_H */
