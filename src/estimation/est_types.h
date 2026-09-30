/* EST-0 semantic contract: observation, belief (state estimate), prediction,
 * innovation, and the linear-Gaussian model they are bound to (ARCH-0020).
 *
 * Standalone module. It is not linked into the resident runtime, the default
 * build, or the default test suite (mk/estimation.mk only). It reads nothing
 * from World, grants nothing, and its object files are checked with nm -u
 * against every authority, World, generation and process entry point.
 *
 * The five categories never collapse:
 *   observation  evidence: a measured value plus its declared noise. Immutable
 *                once recorded. The estimator never writes one.
 *   belief       an estimate: state vector + covariance at a generation,
 *                bound to a model and to the evidence it was derived from.
 *   prediction   a belief propagated forward, plus the predicted observation
 *                distribution. Never an observation, never overwrites one.
 *   innovation   observation minus predicted observation, with its covariance
 *                and normalized squared surprise. A number, never a proof.
 *   model        transition + observation model; identity is its digest.
 * Confidence in any of these is data. Nothing here is an authority input.
 *
 * Records are distinct C types with distinct kind bytes and distinct digest
 * domains. Digest = SHA-256(domain || 0x00 || encoding) using src/sha256.c,
 * the same domain-separation rule as src/turing/ty_qrecord.h. Encoding is a
 * fixed little-endian layout (est_encode_*), doubles as IEEE-754 binary64 bit
 * patterns; the layout is specified in est_types.c above each encoder.
 *
 * Bounded: dimensions are at most EST_MAX_DIM; no allocation; no globals. */
#ifndef OMEGA_EST_TYPES_H
#define OMEGA_EST_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define EST_MAX_DIM 8u
#define EST_DIGEST_SIZE 32u
#define EST_FORMAT_VERSION 1u

/* Digest domains (versioned; a layout change is a new domain). */
#define EST_DOMAIN_MODEL       "omega.est.model.v1"
#define EST_DOMAIN_OBSERVATION "omega.est.observation.v1"
#define EST_DOMAIN_BELIEF      "omega.est.belief.v1"
#define EST_DOMAIN_PREDICTION  "omega.est.prediction.v1"
#define EST_DOMAIN_INNOVATION  "omega.est.innovation.v1"

typedef struct { uint8_t b[EST_DIGEST_SIZE]; } est_digest; /* all-zero = absent */

typedef enum {
    EST_KIND_MODEL = 1,
    EST_KIND_OBSERVATION = 2,
    EST_KIND_BELIEF = 3,
    EST_KIND_PREDICTION = 4,
    EST_KIND_INNOVATION = 5
} est_kind;

/* Units of one state or observation component. Uncertainty (a covariance
 * entry) has the product of the two components' units; a variance has the
 * unit squared. EST_UNIT_NONE is refused: every component declares a unit. */
typedef enum {
    EST_UNIT_NONE = 0,
    EST_UNIT_MILLI_CELSIUS = 1,
    EST_UNIT_MILLI_CELSIUS_PER_S = 2,
    EST_UNIT_WATT = 3,
    EST_UNIT_WATT_PER_S = 4,
    EST_UNIT_NANOSECOND = 5,
    EST_UNIT_BYTE = 6,
    EST_UNIT_DIMENSIONLESS = 7,
    EST_UNIT_LOG2_PICOSECOND = 8,
    EST_UNIT_MAX_ = 9
} est_unit;

/* What a covariance means. v1 accepts only the linear-Gaussian reading:
 * the second central moment of a Gaussian error in the declared units. */
typedef enum {
    EST_UNCERTAINTY_GAUSSIAN_COVARIANCE = 1
} est_uncertainty_meaning;

typedef enum {
    EST_ESTIMATOR_LINEAR_KALMAN = 1
    /* reserved, not implemented: extended, unscented, particle, synthesized */
} est_estimator_kind;

typedef enum {
    EST_OK = 0,
    EST_ERR_NULL = -1,
    EST_ERR_DIM = -2,          /* dimension 0, above EST_MAX_DIM, or mismatch */
    EST_ERR_NONFINITE = -3,    /* NaN or Inf anywhere */
    EST_ERR_ASYMMETRIC = -4,   /* covariance not symmetric within tolerance */
    EST_ERR_NOT_PSD = -5,      /* covariance has a materially negative direction */
    EST_ERR_NOT_PD = -6,       /* matrix that must be invertible is not */
    EST_ERR_UNIT = -7,         /* missing or unknown unit / meaning */
    EST_ERR_STALE = -8,        /* generation or parent digest does not match */
    EST_ERR_MODEL = -9,        /* record bound to a different model */
    EST_ERR_KIND = -10,        /* wrong record kind (e.g. prediction as evidence) */
    EST_ERR_ENCODING = -11,    /* truncated or malformed bytes, bad version */
    EST_ERR_TIME = -12         /* logical time does not advance as required */
} est_status;

/* Row-major n*n matrices; only the first n*n entries are meaningful. */
typedef struct {
    uint32_t n;                  /* state dimension */
    uint32_t m;                  /* observation dimension */
    est_estimator_kind estimator;
    est_uncertainty_meaning meaning;
    est_unit state_unit[EST_MAX_DIM];
    est_unit obs_unit[EST_MAX_DIM];
    double F[EST_MAX_DIM * EST_MAX_DIM]; /* n*n transition per step */
    double Q[EST_MAX_DIM * EST_MAX_DIM]; /* n*n process noise per step */
    double H[EST_MAX_DIM * EST_MAX_DIM]; /* m*n observation matrix */
    double R[EST_MAX_DIM * EST_MAX_DIM]; /* m*m default observation noise */
    int64_t step_ns;             /* logical duration of one transition step */
} est_model;

/* Evidence. value and noise are what the sensor path declared; source names
 * the producer (e.g. digest of "R15.machine-state.thermal_mc[0]"); evidence
 * binds the raw bytes (e.g. the raw SHA256SUMS line or record digest). */
typedef struct {
    uint32_t m;
    est_unit unit[EST_MAX_DIM];
    double z[EST_MAX_DIM];
    double R[EST_MAX_DIM * EST_MAX_DIM];
    int64_t t_ns;                /* logical time of the measurement */
    uint64_t seq;                /* per-source sequence number */
    est_digest source;
    est_digest evidence;         /* must be non-zero */
} est_observation;

/* Estimate. evidence_root chains every observation digest that shaped it:
 * root' = SHA-256(EST_DOMAIN_BELIEF "-root" || 0x00 || root || obs digest). */
typedef struct {
    uint32_t n;
    est_unit unit[EST_MAX_DIM];
    double x[EST_MAX_DIM];
    double P[EST_MAX_DIM * EST_MAX_DIM];
    uint64_t generation;         /* +1 per predict or update */
    int64_t t_ns;                /* logical time the estimate refers to */
    est_digest model;
    est_digest parent;           /* digest of the record it was derived from */
    est_digest evidence_root;    /* zero only for a declared prior */
} est_belief;

/* Prediction: prior belief propagated `horizon` steps, plus the predicted
 * observation distribution y ~ N(y_mean, S). */
typedef struct {
    est_digest prior;            /* digest of the belief it came from */
    est_digest model;
    uint32_t horizon;            /* steps, >= 1 */
    uint64_t generation;
    int64_t t_ns;                /* logical time predicted for */
    uint32_t n, m;
    double x[EST_MAX_DIM];
    double P[EST_MAX_DIM * EST_MAX_DIM];
    double y_mean[EST_MAX_DIM];
    double S[EST_MAX_DIM * EST_MAX_DIM];   /* H P H^T + R */
} est_prediction;

/* Innovation: nu = z - y_mean, S its covariance, nis = nu^T S^-1 nu.
 * Under a calibrated model nis ~ chi-square with m degrees of freedom. */
typedef struct {
    est_digest prediction;
    est_digest observation;
    est_digest model;
    uint32_t m;
    double nu[EST_MAX_DIM];
    double S[EST_MAX_DIM * EST_MAX_DIM];
    double nis;
    int64_t t_ns;
} est_innovation;

/* ---- validation (every constructor and every encoder calls these) ---- */
/* Symmetric within 1e-9 relative to the largest |entry|, every entry finite,
 * and LDL^T has no pivot below -1e-12 * max diagonal (PSD). */
est_status est_check_covariance(const double *A, uint32_t n);
est_status est_check_model(const est_model *mdl);
est_status est_check_observation(const est_observation *obs);
est_status est_check_belief(const est_belief *b);
est_status est_check_prediction(const est_prediction *p);
est_status est_check_innovation(const est_innovation *iv);

/* ---- identity ---- */
est_status est_digest_model(const est_model *mdl, est_digest *out);
est_status est_digest_observation(const est_observation *obs, est_digest *out);
est_status est_digest_belief(const est_belief *b, est_digest *out);
est_status est_digest_prediction(const est_prediction *p, est_digest *out);
est_status est_digest_innovation(const est_innovation *iv, est_digest *out);
est_status est_evidence_root_extend(const est_digest *root, const est_digest *obs,
                                    est_digest *out);
int est_digest_is_zero(const est_digest *d);

/* ---- serialization: exact round trip; decode validates and refuses a kind
 * byte, version or length that does not match the target type ---- */
#define EST_ENCODED_MAX 1400u
est_status est_encode_model(const est_model *mdl, uint8_t *buf, size_t cap, size_t *len);
est_status est_decode_model(const uint8_t *buf, size_t len, est_model *out);
est_status est_encode_observation(const est_observation *o, uint8_t *buf, size_t cap, size_t *len);
est_status est_decode_observation(const uint8_t *buf, size_t len, est_observation *out);
est_status est_encode_belief(const est_belief *b, uint8_t *buf, size_t cap, size_t *len);
est_status est_decode_belief(const uint8_t *buf, size_t len, est_belief *out);
est_status est_encode_prediction(const est_prediction *p, uint8_t *buf, size_t cap, size_t *len);
est_status est_decode_prediction(const uint8_t *buf, size_t len, est_prediction *out);
est_status est_encode_innovation(const est_innovation *iv, uint8_t *buf, size_t cap, size_t *len);
est_status est_decode_innovation(const uint8_t *buf, size_t len, est_innovation *out);

#endif /* OMEGA_EST_TYPES_H */
