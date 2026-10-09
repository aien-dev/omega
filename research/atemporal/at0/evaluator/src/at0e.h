/* AT-0 Agent 4 independent evaluator: shared declarations.
 *
 * Contracts: AT0_CASE_V1 and AT0_RESULT_V1, frozen at aien-architecture
 * 044c9d11256d8642f80eedd42cbae8763faf63f5 (docs/plans/atemporal/).
 * This code shares nothing with src/at0/ (engine, oracle, codec) and never will;
 * the only omega source it links is src/sha256.c (charter section 3).
 *
 * Every decision is made in exact integer arithmetic. binary64 values read from a
 * result are converted to exact dyadic rationals; scaled decimals are exact
 * decimal rationals; both are brought over a common denominator 2^1100 * 10^80
 * and compared as fixed-width big integers (at0e_big). No float is ever compared.
 */
#ifndef AT0E_H
#define AT0E_H

#include <stddef.h>
#include <stdint.h>

#define AT0E_CONTRACT_COMMIT "044c9d11256d8642f80eedd42cbae8763faf63f5"
#define AT0E_CASE_DOMAIN "omega.at0.case.v1"
#define AT0E_ACC_DOMAIN "omega.at0.acceptance.v1"
#define AT0E_VERDICT_DOMAIN_V1 "omega.at0.verdict.v1"
#define AT0E_EVIDENCE_DOMAIN_V1 "omega.at0.evidence.v1"
#define AT0E_VERDICT_DOMAIN_V2 "omega.at0.verdict.v2"
#define AT0E_EVIDENCE_DOMAIN_V2 "omega.at0.evidence.v2"
/* Result contract version in force: V2 (Agent 0 ruling, omega#358 comment 6090404007). V1 results are still parsed and judged under their own tags. */
#define AT0E_RESULT_VERSION_DEFAULT 2

#define AT0E_MAX_N 64
#define AT0E_MAX_M 256
#define AT0E_MAX_LINE 4096
#define AT0E_RAT_LIMIT 1048576

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

/* ---- exact rationals (int128, always reduced, d >= 1) ---- */
typedef struct { i128 n, d; } rat;
rat rat_make(i128 n, i128 d);            /* reduces; aborts on d == 0 */
rat rat_add(rat a, rat b);
rat rat_sub(rat a, rat b);
rat rat_mul(rat a, rat b);
rat rat_neg(rat a);
int rat_cmp(rat a, rat b);
int rat_is_zero(rat a);
int rat_is_int(rat a);
rat rat_mod1(rat a);                     /* a - floor(a), in [0,1) */
int rat_sqrt(rat a, rat *out);           /* 1 if a is the square of a rational (a >= 0) */
int rat_parse(const char *tok, rat *out, int *canonical); /* 1 on parse ok */
void rat_format(rat a, char *buf, size_t cap);

typedef struct { rat re, im; } crat;
int crat_parse(const char *tok, crat *out, int *canonical);
int crat_is_zero(crat z);

/* ---- scaled decimal N@k ---- */
typedef struct { u128 n; int k; } scaled;  /* n < 10^38 fits u128 ; k 0..40 */
int scaled_parse(const char *tok, scaled *out, int *canonical);

/* ---- fixed-width big integers (4096 bits) ---- */
#define BIG_LIMBS 128
typedef struct { uint32_t l[BIG_LIMBS]; } big;
typedef struct { int neg; big m; } sbig;   /* sign-magnitude; zero has neg = 0 */
void big_zero(big *a);
void big_from_u128(big *a, u128 v);
int big_is_zero(const big *a);
int big_cmp(const big *a, const big *b);
void big_add(big *r, const big *a, const big *b);          /* aborts on overflow */
void big_sub(big *r, const big *a, const big *b);          /* requires a >= b */
void big_shl(big *a, unsigned bits);                       /* aborts on overflow */
void big_mul_u32(big *a, uint32_t m);                      /* aborts on overflow */
void big_mul_pow10(big *a, unsigned k);
void sbig_zero(sbig *a);
void sbig_add(sbig *r, const sbig *a, const sbig *b);
void sbig_neg(sbig *a);
void sbig_abs(sbig *a);
int sbig_cmp(const sbig *a, const sbig *b);
/* Common-denominator representation Q = value * 2^1100 * 10^80. */
#define AT0E_DEN_P2 1100u
#define AT0E_DEN_P10 80u
void sbig_from_f64_bits(sbig *q, uint64_t bits);           /* finite only */
void sbig_from_scaled(sbig *q, scaled s);
void sbig_from_int(sbig *q, long v);

/* ---- tri-state comparison of a quantity against a tolerance ---- */
typedef enum { CMP_PASS = 0, CMP_FAIL = 1, CMP_INDET = 2 } tri;
/* absolute form: PASS if |v|+b <= tol, FAIL if |v|-b > tol, else INDET */
tri tri_absolute(const sbig *v, const sbig *b, const sbig *tol);
/* signed form: PASS if v+b <= tol, FAIL if v-b > tol, else INDET */
tri tri_signed(const sbig *v, const sbig *b, const sbig *tol);

/* ---- text / files ---- */
typedef struct {
    char **lines; size_t n;            /* each line without its LF */
    uint8_t *bytes; size_t len;        /* whole file */
} textfile;
/* Reads a file and enforces AT0_CASE_V1 section 1 byte rules. Returns 0 ok,
 * else a nonzero reason code (see at0e_text.c) and *why names the rule. */
int textfile_read(const char *path, textfile *tf, const char **why);
int textfile_from_bytes(const uint8_t *b, size_t len, textfile *tf, const char **why);
void textfile_free(textfile *tf);
int split_tokens(const char *line, char **tok, int max);  /* single-space split; -1 if rule broken */
int is_label(const char *s);
int is_name(const char *s);
int is_digest(const char *s);
int is_sha1hex(const char *s);
int is_text200(const char *s);
int parse_int_canonical(const char *s, long *out);        /* canonical integer text */
void sha256_hex(const uint8_t *data, size_t len, char out[65]);
void sha256_tagged_hex(const char *domain, const uint8_t *data, size_t len, char out[65]);
int hex_to_u64(const char *s16, uint64_t *out);

/* ---- parsed case ---- */
enum { BK_NONE = 0, BK_ESTIMATED = 1, BK_RIGOROUS = 2 };
typedef struct {
    char case_name[80];
    int N;
    rat E[AT0E_MAX_N];
    rat h0, hx, hy, hz;
    char ref_label[40];
    crat psi0[2];
    rat tau, w;
    int M;
    char label[AT0E_MAX_M][40];
    int control_positive;              /* 1 POSITIVE, 0 NEGATIVE */
    int expected_pass;                 /* 1 PASS, 0 FAIL */
    char expected_codes[16][48]; int n_expected_codes;
    int min_bound_kind;
    scaled tol_constraint, tol_povm, tol_prob, tol_zero, tol_schro;
    char case_id[65], acceptance_id[65];
    /* raw blocks, LF-terminated, for identity recomputation */
    char *sem_block; size_t sem_len;
    char *acc_block; size_t acc_len;
    char case_file_sha256[65];
    /* derived (exact) */
    rat radius;                        /* |h|, rational */
    int ref_index;                     /* k of reference label */
} at0_case;

/* Refusal codes (AT0_RESULT_V1 section 5), returned by case_parse_validate. */
typedef enum {
    REFUSE_NONE = 0,
    REFUSE_PARSE_ERROR, REFUSE_NONCANONICAL, REFUSE_UNSUPPORTED_VERSION,
    REFUSE_INVALID_PARAMETER, REFUSE_IRRATIONAL_SPECTRUM, REFUSE_ID_MISMATCH
} refusal;
const char *refusal_name(refusal r);
/* Parses the case lines (already byte-checked). On refusal, *detail names the
 * offending line. The identities are recomputed and compared (rule 5). */
refusal case_parse_validate(const textfile *tf, at0_case *c, const char **detail);
/* Same, on a sequence of lines starting at case_name (used inside results). */
refusal case_parse_blocks(char **lines, size_t n, size_t *consumed, at0_case *c, const char **detail, int in_result);
void case_free(at0_case *c);
/* Writes the canonical case text for a filled-in semantic/acceptance struct,
 * computing both identities. Returns bytes written or -1. */
long case_emit(const at0_case *c, char *buf, size_t cap);
int failure_code_valid(const char *code);

/* ---- shadow oracle (independent physics, long double) ---- */
typedef struct {
    int kernel_dim;                    /* exact */
    int matched[2];                    /* e_+ (index 0) / e_- (index 1) matched by a clock energy */
    int psi_nonzero;                   /* exact: Psi != 0 */
    int povm_exact_identity;           /* exact: sum F_k == I */
    long double povm_residual;         /* numeric Frobenius norm */
    long double clock_p[AT0E_MAX_M];
    int clock_p_exact_zero[AT0E_MAX_M];/* exact: phi_k == 0 */
    long double pauli[AT0E_MAX_M][3][2];   /* [k][axis][sign] from the relational model */
    long double reference[AT0E_MAX_M][3][2];/* Schrodinger reference */
    long double bound;                 /* claimed absolute bound on every numeric above */
} shadow;
int shadow_compute(const at0_case *c, shadow *s, const char **why);

/* ---- parsed result ---- */
typedef struct { int is_undefined, is_nonfinite; uint64_t bits; scaled bound; } rval;
enum { LS_DEFINED = 0, LS_UNDEFINED = 1, LS_INDETERMINATE = 2 };
enum { CK_PASS = 0, CK_FAIL = 1, CK_INDET = 2, CK_NOTEVAL = 3 };
enum { OUT_PASS = 0, OUT_FAIL = 1, OUT_ERROR = 2, OUT_NOT_RUN = 3 };
#define AT0E_NCHECKS 10
extern const char *const at0e_contract_commits[];
extern const char *const at0e_check_names[AT0E_NCHECKS];
extern const char *const at0e_check_codes[AT0E_NCHECKS];
typedef struct {
    at0_case c;
    int version;                       /* 1 or 2, from the header */
    char case_file_sha256[65];
    int arithmetic;                    /* 0 BINARY64 1 INTERVAL 2 COMPENSATED 3 NONE */
    int bound_kind;
    long kernel_dim;
    rval constraint_residual, povm_residual;
    int label_status[AT0E_MAX_M];
    rval clock_p[AT0E_MAX_M];
    rval pauli[AT0E_MAX_M][3][2];
    rval reference[AT0E_MAX_M][3][2];
    int check[AT0E_NCHECKS];
    int outcome;
    char failure_codes[16][48]; int n_failure_codes;
    char error_code[32];
    int expectation_met;               /* 0 YES 1 NO 2 NOT_APPLICABLE */
    char verdict_id[65];
    char source_repo[80], source_commit[48], contract_commit[48], engine_sha256[65];
    char oracle_repo[80], oracle_commit[48], oracle_sha256[65];
    char build_cc[208], build_flags[208], host[208], run_started[32], run_finished[32];
    int source_tree_clean;             /* 1 YES 0 NO */
    int n_artifacts;
    char evidence_digest[65];
    /* raw */
    char *ver_block; size_t ver_len;
    size_t evidence_line_index;        /* index of evidence_digest line */
} at0_result;

/* Structural parse of a result (shape, encodings, embedded case). Returns 0 ok,
 * else sets *finding to an evaluator finding code and *detail. */
int result_parse(const textfile *tf, at0_result *r, const char **finding, const char **detail);
void result_free(at0_result *r);

/* Independent re-derivation of the ten checks, outcome, codes, expectation. */
typedef struct {
    int check[AT0E_NCHECKS];
    int outcome;
    char failure_codes[16][48]; int n_failure_codes;
    int expectation_met;
    int any_label_indeterminate;
} rederived;
void result_rederive(const at0_result *r, rederived *d);

/* Evaluator findings: appended to a list and printed as JSON. */
typedef struct { char code[48]; char detail[320]; } finding;
typedef struct { finding f[256]; int n; } findings;
void findings_add(findings *fs, const char *code, const char *fmt, ...);

/* Full verification of one result file against an optional case file and the
 * shadow oracle. Returns the evaluator status string: PASS, FAIL, INCONCLUSIVE. */
const char *result_verify(const textfile *tf, const at0_result *r, const textfile *case_tf,
                          int use_shadow, findings *fs);

/* Synthetic result writer (self-test fixtures only; never evidence). mutant may
 * be NULL or one of the names listed by synth_mutant_names(). */
long result_synth(const at0_case *c, const uint8_t *case_bytes, size_t case_len,
                  const char *mutant, char *buf, size_t cap, const char **why);
const char *const *synth_mutant_names(int *count);

#endif /* AT0E_H */
