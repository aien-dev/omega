/* AT-0 candidate relational quantum engine (Agent 3, research model).
 *
 * Implements the finite Page-Wootters universe of AT0_CASE_V1 / AT0_RESULT_V1
 * (aien-architecture docs/plans/atemporal/ at commit 044c9d1): a clock of
 * dimension N, one qubit, the constraint H_total = H_C (x) I + I (x) H_S, a
 * covariant discrete clock POVM, and conditional qubit statistics read off by
 * the Born rule after a clock measurement.
 *
 * Rules this header enforces by construction:
 *   - no wall-clock, CPU time, GPU, random or scheduling input anywhere;
 *   - the clock phase is a declared measurement parameter (label k, tau);
 *   - no hidden mutable global state: every function takes explicit objects;
 *   - every failure is an explicit at0_status, never a silent default;
 *   - every array index is bounds-checked against the case limits below.
 *
 * Portable C11 (plus __int128 through __extension__ for exact rationals).
 */
#ifndef AT0_MODEL_H
#define AT0_MODEL_H

#include <complex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- contract limits (AT0_CASE_V1 sections 2 and 3) ---------------------- */
#define AT0_CLOCK_DIM_MIN 2
#define AT0_CLOCK_DIM_MAX 64
#define AT0_LABEL_MAX 256
#define AT0_SYSTEM_DIM 2
#define AT0_RATIONAL_LIMIT 1048576LL          /* contract limit on written tokens */
#define AT0_RAT_REPR_LIMIT 4611686018427387904LL /* 2^62: representability of derived rationals */
#define AT0_SCALED_K_MAX 40
#define AT0_INT_SATURATED 999999999999999999LL /* parsed value of any token longer than 18 digits */
#define AT0_LABEL_LEN 32
#define AT0_NAME_LEN 64
#define AT0_DIGEST_HEX 64
#define AT0_LINE_MAX 4096

/* ---- status codes --------------------------------------------------------- */
typedef enum {
    AT0_OK = 0,
    /* case refusal codes, AT0_RESULT_V1 section 5, in contract order */
    AT0_CASE_PARSE_ERROR,
    AT0_CASE_NONCANONICAL,
    AT0_CASE_UNSUPPORTED_VERSION,
    AT0_CASE_INVALID_PARAMETER,
    AT0_CASE_IRRATIONAL_SPECTRUM,
    AT0_CASE_ID_MISMATCH,
    /* engine errors (never silent) */
    AT0_ERR_ARGUMENT,     /* NULL pointer, index out of range, bad enum */
    AT0_ERR_OVERFLOW,     /* exact arithmetic would overflow its limits */
    AT0_ERR_NONFINITE,    /* a computed binary64 is NaN or infinite */
    AT0_ERR_IO,           /* read or write failure */
    AT0_ERR_INTERNAL
} at0_status;

const char *at0_status_name(at0_status s);
int at0_status_is_refusal(at0_status s);

/* ---- exact arithmetic ----------------------------------------------------- */
__extension__ typedef __int128 at0_i128;

typedef struct { int64_t n; int64_t d; } at0_rat;       /* reduced, d >= 1 */
typedef struct { at0_rat re, im; } at0_crat;             /* complex rational */
typedef struct { uint64_t n; int k; } at0_scaled;        /* n / 10^k */

at0_status at0_rat_make(int64_t n, int64_t d, at0_rat *out);  /* reduces, checks limits */
at0_status at0_rat_add(at0_rat a, at0_rat b, at0_rat *out);
at0_status at0_rat_sub(at0_rat a, at0_rat b, at0_rat *out);
at0_status at0_rat_mul(at0_rat a, at0_rat b, at0_rat *out);
int        at0_rat_cmp(at0_rat a, at0_rat b);                 /* exact sign of a-b */
int        at0_rat_is_zero(at0_rat a);
int        at0_rat_in_limits(at0_rat a);        /* contract limit |n|, d <= 2^20 */
int        at0_scaled_in_limits(at0_scaled s);  /* contract limit k <= 40 */
double     at0_rat_to_double(at0_rat a);                      /* one correctly-rounded division */
/* exact test: is a a square of a rational? if yes, *root is the nonnegative root */
at0_status at0_rat_sqrt_exact(at0_rat a, at0_rat *root, int *is_square);
/* overflow-free exact helpers for in-limit case data (see at0_exact.c): */
int        at0_i128_mul(at0_i128 a, at0_i128 b, at0_i128 *out);    /* 1 on success, 0 on overflow */
int        at0_i128_add(at0_i128 a, at0_i128 b, at0_i128 *out);
at0_status at0_rat_norm_exact(at0_rat hx, at0_rat hy, at0_rat hz, at0_rat *norm, int *is_square); /* |h| */
at0_status at0_exact_norm_plus_hz(at0_rat hx, at0_rat hy, at0_rat hz, at0_rat h_norm,
                                  at0_i128 *A, at0_i128 *D);        /* |h| + hz = A / D, D = dx dy dz */
at0_status at0_exact_sum_is_zero(int count, const at0_i128 *num, const at0_i128 *den, int *zero);
/* reduce x into [-1/2, 1/2) exactly: x - round(x) */
at0_status at0_rat_reduce_turn(at0_rat x, at0_rat *out);

/* parse / print canonical tokens; parsing refuses non-canonical text */
at0_status at0_rat_parse(const char *tok, at0_rat *out);
at0_status at0_crat_parse(const char *tok, at0_crat *out);
at0_status at0_scaled_parse(const char *tok, at0_scaled *out);
int at0_rat_format(at0_rat a, char *buf, size_t cap);
int at0_crat_format(at0_crat a, char *buf, size_t cap);
int at0_scaled_format(at0_scaled s, char *buf, size_t cap);

/* exact comparison of a finite nonnegative binary64 p against scaled decimals:
 * sets *le  = (p + b <= tol), *gt = (p - b > tol), computed with exact integers */
at0_status at0_exact_prob_status(double p, at0_scaled b, at0_scaled tol, int *le, int *gt);

/* binary64 -> smallest-ish scaled decimal >= x (x finite, >= 0); ESTIMATED use only */
at0_status at0_scaled_from_double_ceil(double x, at0_scaled *out);

/* ---- case (versioned input) ----------------------------------------------- */
typedef enum { AT0_CTRL_POSITIVE, AT0_CTRL_NEGATIVE } at0_control_kind;
typedef enum { AT0_EXPECT_PASS, AT0_EXPECT_FAIL } at0_expected_outcome;
typedef enum { AT0_BOUND_NONE = 0, AT0_BOUND_ESTIMATED = 1, AT0_BOUND_RIGOROUS = 2 } at0_bound_kind;

#define AT0_FAILURE_CODE_COUNT 10
extern const char *const at0_failure_codes[AT0_FAILURE_CODE_COUNT]; /* sorted bytewise */

typedef struct {
    char name[AT0_NAME_LEN + 1];
    /* semantic block */
    int clock_dim;                               /* N */
    at0_rat clock_energies[AT0_CLOCK_DIM_MAX];   /* strictly increasing */
    at0_rat h0, hx, hy, hz;
    char reference_clock_label[AT0_LABEL_LEN + 1];
    at0_crat psi0[AT0_SYSTEM_DIM];
    at0_rat povm_tau_turns;
    at0_rat povm_weight;
    int label_count;                             /* M */
    char labels[AT0_LABEL_MAX][AT0_LABEL_LEN + 1];
    /* acceptance block */
    at0_control_kind control_kind;
    at0_expected_outcome expected_outcome;
    int expected_failure_count;                  /* 0 means token "none" */
    int expected_failure_idx[AT0_FAILURE_CODE_COUNT];
    at0_bound_kind min_bound_kind;
    at0_scaled tol_constraint_residual, tol_povm_residual, tol_probability,
               tol_zero_probability, tol_schrodinger;
    /* identities as read from the file, and recomputed */
    char case_id[AT0_DIGEST_HEX + 1];
    char acceptance_id[AT0_DIGEST_HEX + 1];
    char case_file_sha256[AT0_DIGEST_HEX + 1];
    /* derived exact data filled by validation */
    int reference_index;                         /* index r of reference label */
    at0_rat h_norm;                              /* |h| rational */
} at0_case;

/* Parse and validate AT0_CASE_V1 bytes (rules 1..6 in order). On refusal returns
 * the refusal status and leaves *out unspecified. */
at0_status at0_case_parse(const uint8_t *bytes, size_t len, at0_case *out);
/* Emit the canonical case text (byte-exact round trip). Returns bytes written or -1. */
long at0_case_emit(const at0_case *c, char *buf, size_t cap);
/* Compute case_id / acceptance_id from a parsed case into 64-hex buffers. */
at0_status at0_case_identities(const at0_case *c, char case_id[AT0_DIGEST_HEX + 1],
                               char acceptance_id[AT0_DIGEST_HEX + 1]);
at0_status at0_case_read_file(const char *path, at0_case *out);

/* ---- quantum state representation ---------------------------------------- */
/* The combined Hilbert space is C^N (x) C^2 with clock factor first:
 * component index (j, s) -> j * 2 + s. */
#define AT0_TOTAL_DIM_MAX (AT0_CLOCK_DIM_MAX * AT0_SYSTEM_DIM)

typedef struct {
    int clock_dim;
    double complex amp[AT0_TOTAL_DIM_MAX];
} at0_state;

typedef struct { double complex v[AT0_SYSTEM_DIM]; } at0_qubit;
typedef struct { double complex m[AT0_SYSTEM_DIM][AT0_SYSTEM_DIM]; } at0_qubit_op;
typedef struct { int clock_dim; double complex v[AT0_CLOCK_DIM_MAX]; } at0_clock_vec;

at0_status at0_state_init(at0_state *s, int clock_dim);               /* zero state */
at0_status at0_state_get(const at0_state *s, int j, int sys, double complex *out);
at0_status at0_state_set(at0_state *s, int j, int sys, double complex val);
at0_status at0_state_norm2(const at0_state *s, double *out);            /* <s|s> */
at0_status at0_state_inner(const at0_state *a, const at0_state *b, double complex *out);
at0_status at0_state_scale(at0_state *s, double complex z);
at0_status at0_state_check_finite(const at0_state *s);

/* ---- Hamiltonian construction --------------------------------------------- */
typedef struct {
    double eig[AT0_SYSTEM_DIM];         /* binary64 values of e_0 = h0 - |h|, e_1 = h0 + |h|; the exact
                                           spectrum lives in the case (h0, h_norm) and is what the kernel test uses */
    at0_qubit vec[AT0_SYSTEM_DIM];      /* orthonormal eigenvectors (binary64) */
    int degenerate;                     /* |h| == 0 */
    at0_qubit_op hs;                    /* H_S matrix in computational basis */
} at0_system_hamiltonian;

at0_status at0_hamiltonian_system(const at0_case *c, at0_system_hamiltonian *out);
/* apply H_total = H_C (x) I + I (x) H_S to a state */
at0_status at0_hamiltonian_apply_total(const at0_case *c, const at0_system_hamiltonian *hs,
                                       const at0_state *in, at0_state *out);

/* ---- constraint: kernel projection and verification ----------------------- */
typedef struct {
    int kernel_dim;                     /* exact count of (j, s) with E_j + e_s = 0 */
    int match_j[AT0_TOTAL_DIM_MAX];     /* matched clock index */
    int match_s[AT0_TOTAL_DIM_MAX];     /* matched system eigen index */
} at0_kernel;

at0_status at0_constraint_kernel(const at0_case *c, const at0_system_hamiltonian *hs, at0_kernel *out);
/* |Psi> = P_0 (|t_r> (x) |psi_0>) */
at0_status at0_constraint_physical_state(const at0_case *c, const at0_system_hamiltonian *hs,
                                         const at0_kernel *k, at0_state *psi);
/* || H_total Psi_hat ||_2 ; requires Psi nonzero */
at0_status at0_constraint_residual(const at0_case *c, const at0_system_hamiltonian *hs,
                                   const at0_state *psi, double *out);

/* ---- clock POVM ----------------------------------------------------------- */
/* |t_k> = N^(-1/2) sum_j exp(-2 pi i E_j k tau) |E_j>, phase reduced exactly mod one turn */
at0_status at0_povm_clock_state(const at0_case *c, int k, at0_clock_vec *out);
/* || sum_k w |t_k><t_k| - I ||_F */
at0_status at0_povm_residual(const at0_case *c, double *out);

/* ---- conditional subsystem state ------------------------------------------ */
typedef struct {
    at0_qubit phi;              /* (<t_k| (x) I) Psi, unnormalized */
    double phi_norm2;           /* ||phi||^2 */
    double clock_probability;   /* w ||phi||^2 / ||Psi||^2 */
    int defined_numeric;        /* phi_norm2 > 0 so rho exists numerically */
    at0_qubit_op rho;           /* phi phi^dagger / ||phi||^2 when defined */
} at0_conditional;

at0_status at0_conditional_compute(const at0_case *c, const at0_state *psi, double psi_norm2,
                                   int k, at0_conditional *out);

/* ---- observables ---------------------------------------------------------- */
typedef enum { AT0_AXIS_X = 0, AT0_AXIS_Y = 1, AT0_AXIS_Z = 2 } at0_axis;
typedef enum { AT0_SIGN_PLUS = 0, AT0_SIGN_MINUS = 1 } at0_sign;

at0_status at0_pauli_matrix(at0_axis axis, at0_qubit_op *out);
/* P(sigma = +/-1) = Tr(rho (I +/- sigma)/2), computed directly, not as 1 - other */
at0_status at0_observable_probability(const at0_qubit_op *rho, at0_axis axis, at0_sign sign, double *out);

/* ---- engine run and versioned output -------------------------------------- */
typedef enum { AT0_LABEL_DEFINED, AT0_LABEL_UNDEFINED, AT0_LABEL_INDETERMINATE } at0_label_status;

typedef struct {
    at0_label_status status;
    double clock_probability; at0_scaled clock_probability_bound;
    double pauli[3][2];       at0_scaled pauli_bound[3][2];
} at0_label_values;

typedef struct {
    int kernel_dim;
    int psi_nonzero;
    double constraint_residual; at0_scaled constraint_residual_bound;
    double povm_residual;       at0_scaled povm_residual_bound;
    int label_count;
    at0_label_values label[AT0_LABEL_MAX];
    at0_bound_kind bound_kind;  /* always ESTIMATED in this engine */
} at0_engine_result;

at0_status at0_engine_run(const at0_case *c, at0_engine_result *out);
/* test-only: evaluate the labels in reverse order; must give bit-identical values */
at0_status at0_engine_run_reversed(const at0_case *c, at0_engine_result *out);

/* OMEGA-AT0-ENGINE v1 writer (values block identical to AT0_RESULT_V1 section 1
 * minus the oracle's reference lines). Returns bytes written or -1. */
long at0_engine_emit(const at0_case *c, const at0_engine_result *r, char *buf, size_t cap);
int at0_f64_format(double x, char *buf, size_t cap);   /* f64:<16 hex>, -0 folded, nonfinite */

#endif /* AT0_MODEL_H */
