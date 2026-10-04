/* DUAL: constraint pricing for soft resource budgets (ARCH-0031 / ADR 0031).
 *
 * DUAL estimates a nonnegative price (a dual variable) for each DECLARED soft
 * resource budget, from calibrated ESTIMATION records (ARCH-0020), bound to a
 * World generation and an evidence root. Prices order alternatives that have
 * already passed every hard gate and already survived Pareto filtering. They
 * never decide validity, eligibility, promotion or authority.
 *
 *   price    != truth        scarcity != correctness
 *   price    != permission   cheap    != valid
 *
 * This header is the DUAL-0 record contract (ADR 0031 section 4). It is a
 * standalone module: no file under src/runtime/ includes it, it is not part
 * of `all` or `test`, and its objects pass an `nm -u` refusal check against
 * every authority, promotion, World, capability and process symbol
 * (mk/dual.mk, `dual-purity`). Bounded: no allocation, no globals, no I/O,
 * deterministic. Every input is const, every output a new record. Refusals
 * are never silently repaired (fail closed).
 *
 * Classes (section 3). INVARIANT is unconstructible here: a constraint of that
 * class has no lambda field to fill and rx_dual_check_constraint refuses it.
 * CAPACITY carries a diagnostic lambda only and is never relaxed by DUAL.
 * SOFT is priceable. An undeclared class is refused, never defaulted.
 *
 * Records are digested as SHA-256(domain || 0x00 || canonical encoding) with
 * domains "omega.dual.<record>.v1" and fixed little-endian encodings (doubles
 * as IEEE-754 binary64 bits, -0.0 canonicalized to +0.0, NaN/Inf refused
 * before encoding). Each record kind has its own kind byte and digest domain;
 * a decoder refuses a kind byte, version or length that does not match the
 * target type. No wall-clock value appears in any identity: `generation` is
 * the World generation of the bound estimate and `tick` is the controller's
 * logical update index.
 *
 * Naming: the house prefix is RxDual* / rx_dual_*. "Shadow" is not used (omega
 * already has a Shadow structure in rx_graph.c). No physics-themed names. */
#ifndef OMEGA_RX_DUAL_H
#define OMEGA_RX_DUAL_H

#include <stddef.h>
#include <stdint.h>

#define RX_DUAL_DIGEST_SIZE 32u
#define RX_DUAL_FORMAT_VERSION 1u
#define RX_DUAL_MAX_RESOURCES 32u
#define RX_DUAL_MAX_CANDIDATES 64u
#define RX_DUAL_SITE_NAME_SIZE 32u

#define RX_DUAL_DOMAIN_RESOURCE       "omega.dual.resource.v1"
#define RX_DUAL_DOMAIN_CONSTRAINT     "omega.dual.constraint.v1"
#define RX_DUAL_DOMAIN_CONTROLLER     "omega.dual.controller.v1"
#define RX_DUAL_DOMAIN_PRICE_VECTOR   "omega.dual.pricevec.v1"
#define RX_DUAL_DOMAIN_RECOMMENDATION "omega.dual.recommendation.v1"
#define RX_DUAL_DOMAIN_OUTCOME        "omega.dual.outcome.v1"
#define RX_DUAL_DOMAIN_SITE           "omega.dual.site.v1"

typedef struct { uint8_t b[RX_DUAL_DIGEST_SIZE]; } RxDualDigest; /* all-zero = absent */

typedef enum {
    RX_DUAL_KIND_RESOURCE = 1,
    RX_DUAL_KIND_CONSTRAINT = 2,
    RX_DUAL_KIND_CONTROLLER = 3,
    RX_DUAL_KIND_PRICE_VECTOR = 4,
    RX_DUAL_KIND_RECOMMENDATION = 5,
    RX_DUAL_KIND_OUTCOME = 6,
    RX_DUAL_KIND_SITE = 7
} RxDualKind;

/* Unit registry (ADR 0031 section 4.1). Two values are comparable only if
 * their units are identical. Conversion is explicit (rx_dual_unit_from_est in
 * rx_dual_bind.c) and recorded; an implicit conversion is a refusal.
 * RX_DUAL_UNIT_NONE is refused everywhere. */
typedef enum {
    RX_DUAL_UNIT_NONE = 0,
    RX_DUAL_UNIT_PS = 1,                    /* picoseconds */
    RX_DUAL_UNIT_PJ = 2,                    /* picojoules */
    RX_DUAL_UNIT_BYTES = 3,
    RX_DUAL_UNIT_BYTES_PER_S = 4,
    RX_DUAL_UNIT_KV_BLOCKS = 5,
    RX_DUAL_UNIT_PPM_OCCUPANCY = 6,
    RX_DUAL_UNIT_NS_VERIFY = 7,
    RX_DUAL_UNIT_NS_SYNTH = 8,
    RX_DUAL_UNIT_NS = 9,                    /* = EST_UNIT_NANOSECOND */
    RX_DUAL_UNIT_WATT = 10,                 /* = EST_UNIT_WATT */
    RX_DUAL_UNIT_WATT_PER_S = 11,
    RX_DUAL_UNIT_MILLI_CELSIUS = 12,
    RX_DUAL_UNIT_MILLI_CELSIUS_PER_S = 13,
    RX_DUAL_UNIT_DIMENSIONLESS = 14,
    RX_DUAL_UNIT_LOG2_PS = 15,
    RX_DUAL_UNIT_MAX_ = 16
} RxDualUnit;

/* Constraint class, declared by the owning contract, never by DUAL. */
typedef enum {
    RX_DUAL_CLASS_UNDECLARED = 0,  /* refused: never defaulted to SOFT */
    RX_DUAL_CLASS_INVARIANT = 1,   /* refused at construction: unpriceable */
    RX_DUAL_CLASS_CAPACITY = 2,    /* diagnostic lambda only; never relaxed */
    RX_DUAL_CLASS_SOFT = 3,        /* priceable */
    RX_DUAL_CLASS_MAX_ = 4
} RxDualClass;

typedef enum {
    RX_DUAL_EST_MEASURED = 1,   /* estimate_ref is an ARCH-0020 observation */
    RX_DUAL_EST_ESTIMATED = 2,  /* estimate_ref is an ARCH-0020 belief */
    RX_DUAL_EST_PREDICTED = 3,  /* estimate_ref is an ARCH-0020 prediction */
    RX_DUAL_EST_MAX_ = 4
} RxDualEstimateKind;

/* Consumers treat everything but FRESH as absent (ADR 0031 section 5.2, 10). */
typedef enum {
    RX_DUAL_LAMBDA_FRESH = 1,
    RX_DUAL_LAMBDA_STALE = 2,         /* generation too old or evidence root failed; lambda held */
    RX_DUAL_LAMBDA_FROZEN = 3,        /* regime change signalled; lambda held until requalified */
    RX_DUAL_LAMBDA_UNCALIBRATED = 4,  /* no EST-3 calibration receipt; recorded, never production */
    RX_DUAL_LAMBDA_REFUSED = 5,       /* malformed input values; lambda held */
    RX_DUAL_LAMBDA_MAX_ = 6
} RxDualLambdaState;

#define RX_DUAL_AUTHORITY_NONE 0u  /* the only legal RxDualRecommendation.authority */

typedef enum {
    RX_DUAL_OK = 0,
    RX_DUAL_ERR_NULL = -1,
    RX_DUAL_ERR_NONFINITE = -2,    /* NaN or Inf anywhere */
    RX_DUAL_ERR_UNIT = -3,         /* missing, unknown or mismatched unit */
    RX_DUAL_ERR_CLASS = -4,        /* undeclared, INVARIANT or unknown class */
    RX_DUAL_ERR_SCALE = -5,        /* scale not strictly positive */
    RX_DUAL_ERR_RANGE = -6,        /* negative uncertainty/lambda, lambda above max, bad enum, bad index */
    RX_DUAL_ERR_KIND = -7,         /* wrong record kind */
    RX_DUAL_ERR_ENCODING = -8,     /* truncated, trailing bytes, bad version */
    RX_DUAL_ERR_RESOURCE = -9,     /* unknown, duplicate or unordered resource id */
    RX_DUAL_ERR_DIGEST = -10,      /* required digest absent, or parent/contract mismatch */
    RX_DUAL_ERR_GENERATION = -11,  /* generation inconsistent across a record or vector */
    RX_DUAL_ERR_CONTROLLER = -12,  /* controller malformed or does not match controller_id */
    RX_DUAL_ERR_AUTHORITY = -13,   /* recommendation claims authority */
    RX_DUAL_ERR_SITE = -14,        /* decision site malformed or does not match */
    RX_DUAL_ERR_FULL = -15,        /* registry or vector capacity */
    RX_DUAL_ERR_EVIDENCE = -16,    /* evidence root does not verify */
    RX_DUAL_ERR_CALIBRATION = -17  /* FRESH claimed without a calibration receipt */
} RxDualStatus;

/* ---- 4.1 resource registry entry ---- */
typedef struct {
    uint32_t resource_id;
    RxDualUnit unit;
    double scale;             /* positive normalizer in `unit`; makes pressure dimensionless */
    RxDualDigest contract;    /* non-zero: the contract that declared this resource */
} RxDualResource;

/* Registry: bounded, kept in strictly ascending resource_id order. */
typedef struct {
    uint32_t n;
    RxDualResource r[RX_DUAL_MAX_RESOURCES];
} RxDualRegistry;

/* ---- 4.2 constraint state ---- */
typedef struct {
    uint32_t resource_id;
    RxDualUnit unit;
    RxDualClass cls;                 /* CAPACITY or SOFT only */
    double budget;                   /* in `unit` */
    RxDualDigest budget_contract;    /* non-zero: digest of the contract that declared budget and class */
    RxDualDigest observation_ref;    /* ARCH-0020 observation digest; non-zero when estimate_kind == MEASURED */
    RxDualDigest estimate_ref;       /* non-zero: ARCH-0020 observation/belief/prediction digest */
    RxDualEstimateKind estimate_kind;
    double estimate;                 /* copied from the referenced record, never computed by DUAL */
    double uncertainty;              /* standard deviation in `unit`, >= 0, copied likewise */
    RxDualDigest calibration_ref;    /* EST-3 receipt digest; zero means UNCALIBRATED */
    double lambda;                   /* finite, >= 0, <= controller lambda_max */
    RxDualLambdaState lambda_state;
    RxDualDigest controller_id;      /* non-zero: rx_dual_digest_controller of the producing controller */
    uint64_t generation;             /* World generation of the bound estimate */
    uint64_t tick;                   /* controller logical update index; 0 for the initial state */
    RxDualDigest evidence_root;      /* non-zero: chain over every estimate record consumed */
    RxDualDigest parent;             /* previous state digest; zero only when tick == 0 */
} RxDualConstraintState;

/* ---- controller parameters (section 5.1); their digest is controller_id ---- */
typedef struct {
    double eta;        /* step, >= 0 finite (0 = no gradient movement) */
    double rho;        /* leak toward zero, in [0, 1] */
    double k_sigma;    /* deadband width in standard deviations, >= 0 finite */
    uint64_t max_age;  /* maximum (now - estimate generation) accepted, >= 1 */
    uint64_t cadence;  /* update cadence in ticks, >= 1 */
    uint32_t n;        /* resources this controller prices, 1..RX_DUAL_MAX_RESOURCES */
    uint32_t resource_id[RX_DUAL_MAX_RESOURCES];  /* strictly ascending */
    double lambda_max[RX_DUAL_MAX_RESOURCES];     /* > 0 finite, per resource */
} RxDualController;

/* ---- 4.3 price vector: one decision context, one generation ---- */
typedef struct {
    uint64_t generation;
    RxDualDigest context;            /* non-zero: the decision context (site digest or caller record) */
    uint32_t n;                      /* 1..RX_DUAL_MAX_RESOURCES */
    uint32_t resource_id[RX_DUAL_MAX_RESOURCES];  /* strictly ascending */
    RxDualDigest state[RX_DUAL_MAX_RESOURCES];    /* RxDualConstraintState digests, non-zero */
} RxDualPriceVector;

/* ---- registered decision site (section 7.3) ---- */
typedef struct {
    uint32_t site_id;
    uint8_t name[RX_DUAL_SITE_NAME_SIZE];  /* printable ASCII, NUL padded, e.g. "jspace.residency" */
    RxDualDigest owner;                    /* non-zero: digest naming the production owner */
    uint32_t max_alternatives;             /* 1..RX_DUAL_MAX_CANDIDATES */
} RxDualSite;

/* ---- 4.4 recommendation (advisory; authority is always NONE) ---- */
typedef struct {
    uint32_t resource_id;
    RxDualUnit unit;
    double predicted;        /* predicted consequence of `recommended`, in `unit` */
    double predicted_sd;     /* > 0 when has_measured, else >= 0 */
    uint32_t has_measured;   /* 0 or 1 */
    double measured;         /* 0 when has_measured == 0 */
    double error;            /* (measured - predicted) / predicted_sd when has_measured, else 0 */
} RxDualConsequence;

typedef struct {
    RxDualDigest decision_site;      /* non-zero: RxDualSite digest */
    RxDualDigest price_vector;       /* non-zero: RxDualPriceVector digest */
    uint64_t generation;
    uint32_t n_candidates;           /* 1..RX_DUAL_MAX_CANDIDATES */
    RxDualDigest candidates[RX_DUAL_MAX_CANDIDATES];  /* non-zero, nondominated alternatives offered */
    uint32_t actual;                 /* index of the production choice */
    uint32_t recommended;            /* index DUAL would have chosen */
    uint32_t n_consequences;         /* 0..RX_DUAL_MAX_RESOURCES */
    RxDualConsequence c[RX_DUAL_MAX_RESOURCES];        /* strictly ascending resource_id */
    uint32_t authority;              /* RX_DUAL_AUTHORITY_NONE; anything else is malformed */
} RxDualRecommendation;

/* ---- 4.5 outcome ---- */
typedef struct {
    RxDualDigest recommendation;
    RxDualDigest before_vector;
    RxDualDigest after_vector;
    uint32_t target_resource_id;
    uint64_t generation_before;
    uint64_t generation_after;       /* >= generation_before */
    double lambda_before;
    double lambda_after;             /* both finite, >= 0 */
} RxDualOutcome;

/* ---- helpers ---- */
int rx_dual_digest_is_zero(const RxDualDigest *d);
int rx_dual_digest_eq(const RxDualDigest *a, const RxDualDigest *b);
int rx_dual_unit_valid(RxDualUnit u);                 /* 1 if known and not NONE */

/* ---- registry ---- */
void rx_dual_registry_init(RxDualRegistry *reg);
RxDualStatus rx_dual_registry_add(RxDualRegistry *reg, const RxDualResource *r);  /* refuses duplicate id */
const RxDualResource *rx_dual_registry_find(const RxDualRegistry *reg, uint32_t resource_id);

/* ---- validation (every constructor, digest and encoder calls these) ---- */
RxDualStatus rx_dual_check_resource(const RxDualResource *r);
RxDualStatus rx_dual_check_constraint(const RxDualConstraintState *s);
RxDualStatus rx_dual_check_controller(const RxDualController *c);
RxDualStatus rx_dual_check_price_vector(const RxDualPriceVector *v);
RxDualStatus rx_dual_check_site(const RxDualSite *s);
RxDualStatus rx_dual_check_recommendation(const RxDualRecommendation *r);
RxDualStatus rx_dual_check_outcome(const RxDualOutcome *o);

/* Constraint against its registry entry: id, unit and class rules. */
RxDualStatus rx_dual_check_constraint_against(const RxDualConstraintState *s, const RxDualResource *r);
/* lambda_max for a resource under a controller; RX_DUAL_ERR_RESOURCE if not priced by it. */
RxDualStatus rx_dual_controller_lambda_max(const RxDualController *c, uint32_t resource_id, double *out);

/* ---- identity ---- */
RxDualStatus rx_dual_digest_resource(const RxDualResource *r, RxDualDigest *out);
RxDualStatus rx_dual_digest_constraint(const RxDualConstraintState *s, RxDualDigest *out);
RxDualStatus rx_dual_digest_controller(const RxDualController *c, RxDualDigest *out);
RxDualStatus rx_dual_digest_price_vector(const RxDualPriceVector *v, RxDualDigest *out);
RxDualStatus rx_dual_digest_site(const RxDualSite *s, RxDualDigest *out);
RxDualStatus rx_dual_digest_recommendation(const RxDualRecommendation *r, RxDualDigest *out);
RxDualStatus rx_dual_digest_outcome(const RxDualOutcome *o, RxDualDigest *out);

/* Price vector builder: every state must validate, share `generation`, and be
 * supplied in strictly ascending resource_id order (canonical; unordered or
 * duplicate input is refused, not sorted). */
RxDualStatus rx_dual_price_vector_build(const RxDualConstraintState *const *states, uint32_t n,
                                        const RxDualDigest *context, RxDualPriceVector *out);

/* Fill one consequence row; computes `error` exactly as the checker recomputes it. */
RxDualStatus rx_dual_consequence_set(RxDualConsequence *c, uint32_t resource_id, RxDualUnit unit,
                                     double predicted, double predicted_sd,
                                     int has_measured, double measured);

/* ---- serialization: exact round trip; decode validates and refuses a kind
 * byte, version, truncation or trailing bytes ---- */
#define RX_DUAL_ENCODED_MAX 4096u
RxDualStatus rx_dual_encode_resource(const RxDualResource *r, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_resource(const uint8_t *buf, size_t len, RxDualResource *out);
RxDualStatus rx_dual_encode_constraint(const RxDualConstraintState *s, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_constraint(const uint8_t *buf, size_t len, RxDualConstraintState *out);
RxDualStatus rx_dual_encode_controller(const RxDualController *c, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_controller(const uint8_t *buf, size_t len, RxDualController *out);
RxDualStatus rx_dual_encode_price_vector(const RxDualPriceVector *v, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_price_vector(const uint8_t *buf, size_t len, RxDualPriceVector *out);
RxDualStatus rx_dual_encode_site(const RxDualSite *s, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_site(const uint8_t *buf, size_t len, RxDualSite *out);
RxDualStatus rx_dual_encode_recommendation(const RxDualRecommendation *r, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_recommendation(const uint8_t *buf, size_t len, RxDualRecommendation *out);
RxDualStatus rx_dual_encode_outcome(const RxDualOutcome *o, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_outcome(const uint8_t *buf, size_t len, RxDualOutcome *out);

#endif /* OMEGA_RX_DUAL_H */
