/* AT-1 candidate engine (Agent 3, aien-dev/omega#371). Portable C11, libc + libm only.
 *
 * Contracts (frozen, aien-architecture cbe4c8e): AT1_CASE_V1 (sha256 e62018d8...),
 * AT1_RESULT_V1 (sha256 3e6efd7e...), with AT0_RESULT_V2 sections 2, 4, 5, 7 normative by
 * reference. Mathematics: AT1_SPEC.md at aien-architecture 81047f5. Contract readings (c), (d),
 * (e) of AT1_CHARTER.md section 8 are applied.
 *
 * Layers, one file each:
 *   at1_bn.c       arbitrary-precision integers, exact rationals, Gaussian rationals
 *   at1_case.c     AT1_CASE_V1 parser and validator (rules 1 to 6 in contract order), identities
 *   at1_exact.c    literal H_total as an exact Gaussian-rational matrix, exact nullspace by
 *                  Gauss-Jordan elimination, exact orthogonal projector P_0, exact zero test
 *   at1_numeric.c  binary64 physics: clock states, Psi = P_0 (|t_r> (x) psi_0) by linearity from the
 *                  exact per-level vectors P_0 (|E_j> (x) psi_0), residuals,|t_r> (x) psi_0), residuals,
 *                  phi_k, p(k), rho_k, Pauli outcomes, ESTIMATED bounds
 *   at1_result.c   oracle reference splice, provenance, exact verdict (12 checks), AT1_RESULT_V1
 *                  writer, verdict_id and evidence_digest
 *   at1_io.c       the only file-system access (read a whole file)
 *   at1_main.c     command-line tool
 * No global mutable state. No clock, timer, random source, thread or network call anywhere. */
#ifndef AT1_MODEL_H
#define AT1_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* ---- contract constants ---------------------------------------------------------------- */
#define AT1_CLOCK_DIM_MIN 2
#define AT1_CLOCK_DIM_MAX 64
#define AT1_LABEL_MAX 256
#define AT1_TOKEN_LIMIT 1048576          /* |n| <= 2^20, 1 <= d <= 2^20 */
#define AT1_SCALED_K_MAX 40
#define AT1_LABEL_LEN 32
#define AT1_NAME_LEN 64
#define AT1_FILE_MAX (1u << 20)          /* engine limit: case and auxiliary files up to 1 MiB */
#define AT1_TOKEN_DIGITS_MAX 4096        /* engine limit: numeric tokens up to 4096 digits */

typedef enum {
    AT1_OK = 0,
    /* refusal codes, AT1_RESULT_V1 section 5, in validation order */
    AT1_CASE_PARSE_ERROR,
    AT1_CASE_NONCANONICAL,
    AT1_CASE_UNSUPPORTED_VERSION,
    AT1_CASE_INVALID_PARAMETER,
    AT1_CASE_IRRATIONAL_SPECTRUM,
    AT1_CASE_ID_MISMATCH,
    /* engine errors (never a refusal) */
    AT1_ERR_RESOURCE,                    /* an engine limit (file size, token length) */
    AT1_ERR_INTERNAL,                    /* an internal consistency check failed */
    AT1_ERR_ARGUMENT                     /* bad tool input (oracle record, provenance) */
} at1_status;

const char *at1_status_name(at1_status s);

/* ---- arbitrary-precision integers (at1_bn.c) --------------------------------------------- */
/* Sign-magnitude, 32-bit limbs, little endian; zero has n = 0 and neg = 0. Every output may
 * alias an input. Allocation failure prints AT1_ENGINE_ERROR RESOURCE_LIMIT and exits 1. */
typedef struct { int neg; int n; int cap; uint32_t *d; } bn;

void bn_init(bn *a);
void bn_free(bn *a);
void bn_copy(bn *r, const bn *a);
void bn_set_i64(bn *r, int64_t v);
void bn_set_u64(bn *r, uint64_t v);
int bn_is_zero(const bn *a);
int bn_sign(const bn *a);                                 /* -1, 0, +1 */
int bn_cmp(const bn *a, const bn *b);
int bn_cmp_abs(const bn *a, const bn *b);
void bn_add(bn *r, const bn *a, const bn *b);
void bn_sub(bn *r, const bn *a, const bn *b);
void bn_mul(bn *r, const bn *a, const bn *b);
void bn_neg(bn *r, const bn *a);
void bn_abs(bn *r, const bn *a);
void bn_divmod(bn *q, bn *rem, const bn *a, const bn *b);  /* truncated; q or rem may be NULL */
void bn_gcd(bn *r, const bn *a, const bn *b);              /* r >= 0 */
void bn_shl(bn *r, const bn *a, unsigned bits);
void bn_pow10(bn *r, unsigned k);
int bn_bitlen(const bn *a);
int bn_isqrt_exact(const bn *a, bn *root);                 /* 1 if a >= 0 is a perfect square */
int bn_fits_i64(const bn *a, int64_t *out);
/* decimal digit string (no sign) into r; returns 0 if longer than AT1_TOKEN_DIGITS_MAX */
int bn_from_digits(bn *r, const char *s, size_t len);

/* ---- exact rationals (reduced, den > 0) --------------------------------------------------- */
typedef struct { bn num, den; } bq;

void bq_init(bq *a);
void bq_free(bq *a);
void bq_copy(bq *r, const bq *a);
void bq_set_i64(bq *r, int64_t n, int64_t d);              /* d != 0 */
void bq_set_bn(bq *r, const bn *n, const bn *d);           /* d != 0 */
int bq_is_zero(const bq *a);
int bq_sign(const bq *a);
int bq_cmp(const bq *a, const bq *b);
void bq_add(bq *r, const bq *a, const bq *b);
void bq_sub(bq *r, const bq *a, const bq *b);
void bq_mul(bq *r, const bq *a, const bq *b);
void bq_div(bq *r, const bq *a, const bq *b);              /* b != 0 */
void bq_neg(bq *r, const bq *a);
void bq_abs(bq *r, const bq *a);
double bq_to_double(const bq *a);                          /* correctly rounded (normal range) */
void bq_from_double(bq *r, double x);                      /* exact; x finite */
void bq_from_scaled(bq *r, const bn *n, unsigned k);       /* n / 10^k */

/* ---- exact Gaussian rationals ------------------------------------------------------------- */
typedef struct { bq re, im; } gq;

void gq_init(gq *a);
void gq_free(gq *a);
void gq_copy(gq *r, const gq *a);
int gq_is_zero(const gq *a);
void gq_add(gq *r, const gq *a, const gq *b);
void gq_sub(gq *r, const gq *a, const gq *b);
void gq_mul(gq *r, const gq *a, const gq *b);
void gq_div(gq *r, const gq *a, const gq *b);              /* b != 0 */
void gq_conj(gq *r, const gq *a);
void gq_neg(gq *r, const gq *a);

/* ---- parsed case (at1_case.c) -------------------------------------------------------------- */
typedef enum { AT1_TARGET_IDEAL = 0, AT1_TARGET_INTERACTING = 1 } at1_target;
typedef enum { AT1_BOUND_NONE = 0, AT1_BOUND_ESTIMATED = 1, AT1_BOUND_RIGOROUS = 2 } at1_bound_kind;
typedef enum { AT1_EXPECT_PASS = 0, AT1_EXPECT_FAIL = 1 } at1_expect;

#define AT1_FAILURE_CODE_COUNT 12
extern const char *const at1_failure_codes[AT1_FAILURE_CODE_COUNT];   /* sorted bytewise */

/* a scaled decimal N@k, kept exactly */
typedef struct { bn n; unsigned k; } at1_scaled;

typedef struct {
    /* exact semantic content */
    char name[AT1_NAME_LEN + 1];
    int clock_dim;                        /* N */
    bq energies[AT1_CLOCK_DIM_MAX];
    bq h0, h[3];                          /* system_hamiltonian_pauli h0,hx,hy,hz */
    bq v[AT1_CLOCK_DIM_MAX][3];           /* interaction_pauli j vx,vy,vz */
    int ref_index;                        /* r */
    char ref_label[AT1_LABEL_LEN + 1];
    gq psi0[2];
    bq tau, weight;
    int label_count;                      /* M */
    char labels[AT1_LABEL_MAX][AT1_LABEL_LEN + 1];
    /* acceptance */
    int control_positive;
    at1_target target;
    at1_expect expected_outcome;
    unsigned expected_codes;              /* bit i = at1_failure_codes[i] */
    at1_bound_kind min_bound_kind;
    at1_scaled tol_constraint, tol_povm, tol_probability, tol_zero, tol_schrodinger;
    /* identities (lowercase hex) and the copied text blocks */
    char case_id[65], acceptance_id[65], case_file_sha256[65];
    char *semantic_text; size_t semantic_len;      /* "begin semantic\n" .. "end semantic\n" */
    char *acceptance_text; size_t acceptance_len;  /* "begin acceptance\n" .. "end acceptance\n" */
    int initialized;
} at1_case;

void at1_case_free(at1_case *c);
/* Parse and validate case bytes (AT1_CASE_V1 section 4, first failure in order). On a refusal
 * the case is freed and the refusal status returned. AT1_ERR_RESOURCE for engine limits. */
at1_status at1_case_parse(const uint8_t *bytes, size_t len, at1_case *c);
/* domain-tagged SHA-256 as lowercase hex (tag || 0x00 || data) */
void at1_tagged_sha256(const char *tag, const char *data, size_t len, char out[65]);
void at1_sha256_hex(const uint8_t *data, size_t len, char out[65]);

/* ---- exact kernel (at1_exact.c) ------------------------------------------------------------ */
typedef struct {
    int dim;                              /* D = 2N */
    int kernel_dim;                       /* D - rank(H_total), exact */
    int psi_zero;                         /* Psi exactly zero (exact decision) */
    int levels_in_psi;                    /* L: clock levels j with P_0 (e_j (x) psi_0) != 0 */
    double *h_re, *h_im;                  /* H_total rounded to binary64, D x D row major */
    double *y_re, *y_im;                  /* Y = sum_j P_0 (|E_j> (x) psi_0), exact, rounded once (D) */
    int *owner;                           /* clock level j whose y_j covers entry i, or -1 (D) */
    double hmax;                          /* max_i sum over l with owner[l] >= 0 of |H_total[i,l]| */
} at1_kernel;

void at1_kernel_free(at1_kernel *k);
at1_status at1_kernel_build(const at1_case *c, at1_kernel *k);

/* ---- numerics (at1_numeric.c) --------------------------------------------------------------- */
typedef enum { AT1_LABEL_DEFINED = 0, AT1_LABEL_UNDEFINED = 1, AT1_LABEL_INDETERMINATE = 2 } at1_label_status;

typedef struct {
    double value, bound;                  /* bound >= 0, ESTIMATED */
    int undefined;                        /* write "undefined 0@0" */
} at1_val;

typedef struct {
    int kernel_dim;
    int trivial;                          /* kernel_dim == 0 or Psi exactly zero */
    at1_val constraint, povm;
    int M;
    at1_label_status status[AT1_LABEL_MAX];
    at1_val clock[AT1_LABEL_MAX];
    at1_val pauli[AT1_LABEL_MAX][6];      /* X+, X-, Y+, Y-, Z+, Z- */
} at1_values;

/* reversed != 0 evaluates the labels in reverse order (test T8; values must be bit-identical) */
at1_status at1_compute(const at1_case *c, const at1_kernel *k, int reversed, at1_values *out);
/* label status rule (AT1_RESULT_V1 section 3) decided exactly on the binary64 value and the
 * scaled-decimal bound written for it */
at1_label_status at1_status_rule(double p, double bound, const at1_scaled *tol_zero);
/* smallest scaled decimal >= x (x >= 0 finite), at most 7 significant digits; "0@0" for 0 */
void at1_bound_format(double x, char *buf, size_t cap);
void at1_f64_format(double x, char *buf, size_t cap);

/* ---- result (at1_result.c) ------------------------------------------------------------------- */
/* Writes the complete AT1_RESULT_V1 file into a malloc'd buffer (*out, *out_len).
 * oracle_text == NULL means the oracle is unavailable: outcome ERROR ORACLE_UNAVAILABLE.
 * Returns AT1_ERR_ARGUMENT with a message in err (cap bytes) for a malformed oracle record
 * or provenance file. */
at1_status at1_result_write(const at1_case *c, const at1_values *v,
                            const char *oracle_text, size_t oracle_len,
                            const char *prov_text, size_t prov_len,
                            char **out, size_t *out_len, char *err, size_t cap);
/* an ERROR result with the given run error code (INTERNAL_ERROR, RESOURCE_LIMIT) */
at1_status at1_result_write_error(const at1_case *c, const char *error_code,
                                  const char *prov_text, size_t prov_len,
                                  char **out, size_t *out_len, char *err, size_t cap);
/* engine-only values lines (component output for debugging; not an interface) */
at1_status at1_values_write(const at1_case *c, const at1_values *v, char **out, size_t *out_len);

/* ---- io (at1_io.c) ------------------------------------------------------------------------- */
at1_status at1_read_file(const char *path, uint8_t **bytes, size_t *len);

/* ---- allocation helpers (at1_bn.c) --------------------------------------------------------- */
void *at1_xmalloc(size_t n);
void *at1_xcalloc(size_t n, size_t sz);
void *at1_xrealloc(void *p, size_t n);

#endif
