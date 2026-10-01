/* EST-2/EST-3 replay library (protocol v1 and v2, docs/estimation/EST23_PROTOCOL_V1.md, EST23_PROTOCOL_V2.md).
 * Shared parsing, SHA-256 file verification, and one strictly causal forward
 * pass over a machine-state.ndjson file with model M0 or M1. Tools only: it
 * may open files and print. The filter itself is src/estimation/est_kf.c. */
#ifndef OMEGA_EST_REPLAY_H
#define OMEGA_EST_REPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "est_kf.h"
#include "est_mix.h"
#include "est_types.h"

#define EST_PROTOCOL_COMMIT "f96dc97"
#define EST_RUN_A_ID "20260929T020536Z-ad8e1f2ea4e4-silicon"
#define EST_RUN_B_ID "3e9e53be3358"
#define EST_RUN_A_SHA "6df4a2b079482b79c42da84da5627b6a8fb9c7d3ce885aac53437b0199d517db"
#define EST_BURN_IN 30u
#define EST_NQ 19
#define EST_NR 25
#define EST_PROTOCOL_DOC_SHA "dbd2a407a8cf12d353f9abff5d26985fb2f9d96f6d33872896169082c6082676"
#define EST_SOURCE_TEXT "R15.machine-state.thermal_mc[0]"

/* Run B identity (from that run's SHA256SUMS text only; no data file is read to know these). */
#define EST_RUN_B_RAW_SHA_DEFAULT "91a5fe34225926cd7aca2fca4be5ed51dc0772341a2fda5f9a39d0f72b8a6bb5"
#define EST_RUN_B_MARKS_SHA_DEFAULT "d2dcc89fb1f9bfff15e1dce6be5248ba6f285cfe1fb7dd051c09764aec844d04"
const char *est_run_b_id(void);
const char *est_run_b_raw_sha(void);
const char *est_run_b_marks_sha(void);
/* Nonzero only when est_eval --recorded has proven both inputs are run B. */
extern int est_allow_run_b;
/* 1 when the resolved (realpath) path has run B's id in any component. */
int est_path_is_run_b(const char *path);
/* Find "<hex>  <name>" in dir/SHA256SUMS (text only). 0 when found. */
int est_sums_lookup(const char *dir, const char *name, char hex[65]);

/* Whole file in memory, verified against the SHA256SUMS line beside it. */
typedef struct {
    char path[1024];
    uint8_t *data;
    size_t len;
    size_t nlines;
    size_t *off;
    size_t *llen;
    char sha_hex[65];
} est_file;

/* Loads path and refuses (returns nonzero, message in err) when the file's
 * SHA-256 differs from its line in SHA256SUMS in the same directory, or when
 * there is no such line. */
int est_file_load(const char *path, est_file *f, char *err, size_t errcap);
/* Same reader without SHA256SUMS or the run B guard (used for temp copies). */
int est_file_load_raw(const char *path, est_file *f, char *err, size_t errcap);
void est_file_free(est_file *f);
/* Hex SHA-256 of a whole file on disk (0 on success). */
int est_sha_file_hex(const char *path, char out[65]);
void est_hex(const uint8_t *d, size_t n, char *out);

/* Parsers. Return 0 on success, nonzero when the line does not parse. */
int est_parse_t(const char *line, size_t len, int64_t *t_ns);
int est_parse_thermal0(const char *line, size_t len, double *value);
/* "<sec>.<9 digits>" at the start of s (used for marks lines). Returns 2 when
 * the seconds field has more than 10 digits or would overflow int64 ns. */
int est_parse_time_prefix(const char *s, size_t len, int64_t *t_ns, size_t *used);

double est_grid_q(int i);
double est_grid_r(int j);

int est_make_model(int model_id, double q, double r, est_model *out);

typedef struct {
    size_t line;            /* absolute 0-based line index in the file */
    size_t L;               /* 0-based index from the first valid line */
    int prior;              /* 1 for the declared prior step */
    int coast;              /* 1 when the observation was missing */
    int t_ok;               /* raw t parsed */
    int chain_ok;           /* obs -> prediction -> innovation chain verified */
    int64_t wall_ns;        /* raw t (integer ns), 0 when !t_ok */
    int64_t t_ns;           /* logical grid time */
    uint32_t horizon;
    double z, y_mean, S, nu, nis;
    est_digest evidence, obs_d, pred_d, innov_d, belief_d;
    est_belief post;
} est_step;

typedef struct {
    est_step *steps;
    size_t nsteps, cap;
    size_t lines_used;       /* lines consumed (limit applied) */
    size_t leading_missing;  /* lines before the first valid observation */
    size_t coasts;           /* missing observations after the first valid */
    size_t bad_value, bad_t; /* diagnostics, over all lines used */
    size_t bad_t_overflow;   /* t with too many seconds digits (subset of bad_t) */
    size_t gap_zero, gap_backward; /* duplicate / backward t between parsed lines */
    size_t multi_horizon;    /* steps with horizon > 1 */
    est_model model;
    est_digest model_d;
    int error;               /* nonzero: filter refused something */
    char errmsg[160];
} est_replay;

/* One causal pass over the first `limit` lines (SIZE_MAX = all). Reuses
 * out->steps storage when out was used before (zero-initialise once). */
int est_replay_run(const est_file *f, size_t limit, int model_id, double q, double r,
                   est_replay *out);
void est_replay_free(est_replay *rp);
void est_replay_write_stream(FILE *fp, const est_replay *rp, int model_id);

/* Fit selection: best finite log-likelihood, strict >, q outer / r inner, so a
 * tie keeps the smaller q then the smaller r. Returns 1 when none is finite. */
int est_fit_pick(const double ll[EST_NQ][EST_NR], int *bi, int *bj);

/* Parameter file (text). */
typedef struct {
    char fit_path[1024];
    char fit_sha[65];
    size_t fit_lines;
    double q[2], r[2], ll[2];
} est_params;
int est_params_write(const char *path, const est_params *p);
int est_params_read(const char *path, est_params *p);
/* Protocol checks: q and r finite and on the 19x25 grid (1e-9 relative). */
int est_params_validate(const est_params *p, char *err, size_t cap);


/* ---- Protocol v2 (docs/estimation/EST23_PROTOCOL_V2.md) ----
 * M2: M0's structure with q = total variance of a fitted noise shape and
 * r = EST_M2_R, so the predicted mean is the previous observation exactly
 * (persistence) and the predicted change has the shape in est_mix.h.
 * Fit run C1 and held-out run C2 are fresh 1 Hz collections (EST-3b). */
#define EST_V2_FIT_TAG "-est3b-fit-"
#define EST_V2_HELDOUT_TAG "-est3b-heldout-"
#define EST_M2_R 1e-12
#define EST_M2_FLOOR (100.0 * 100.0 / 12.0)   /* 100 mC sensor step: uniform quantization variance */
#define EST_M2_K 3u
#define EST_M2_ITERS 1000u
/* C1 identity, fixed when C1 was committed (SHA256SUMS line beside it). */
#define EST_V2_FIT_SHA "65252bae5f9d49d30a3b334fd2b9444c36db7627a45fcd9b6ef0bb4e5a7e5716"
/* C2 identity (SHA256SUMS text beside it) and the frozen v2 protocol document. */
#define EST_V2_HELDOUT_RAW_SHA "pending"
#define EST_V2_HELDOUT_MARKS_SHA "pending"
#define EST_V2_PROTOCOL_DOC_SHA "86b7b46f02dd6e51db78cc9f450aac27be189cdb1484894e923a1898429536f5"
#define EST_V2_PROTOCOL_DOC "docs/estimation/EST23_PROTOCOL_V2.md"
/* Nonzero only when est_eval --protocol-v2 --recorded has proven the inputs are C2. */
extern int est_allow_heldout;
/* 1 when the path or its resolved path names the held-out run. */
int est_path_is_heldout(const char *path);

typedef struct {
    char v1_params_path[1024];
    char v1_params_sha[65];
    char fit_path[1024];
    char fit_sha[65];
    size_t fit_lines;
    size_t fit_n;              /* one-step changes used by the EM fit */
    double q, r, ll;           /* M2 q (= total shape variance), r, EM log-likelihood */
    est_mix mix;
} est_params2;
int est_params2_write(const char *path, const est_params2 *p);
/* Exactly the format written by est_params2_write, nothing else. */
int est_params2_read(const char *path, est_params2 *p);
/* M2 fit data: one-step changes (horizon 1, observed, L >= burn-in) of a
 * replay with model 2. Fills e (cap entries) and returns the count; returns
 * (size_t)-1 when the replay mean is not exactly the previous observation. */
size_t est_m2_changes(const est_replay *rp, double *e, size_t cap);

#endif
