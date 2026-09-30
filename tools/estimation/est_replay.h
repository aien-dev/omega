/* EST-2/EST-3 replay library (protocol v1, docs/estimation/EST23_PROTOCOL_V1.md).
 * Shared parsing, SHA-256 file verification, and one strictly causal forward
 * pass over a machine-state.ndjson file with model M0 or M1. Tools only: it
 * may open files and print. The filter itself is src/estimation/est_kf.c. */
#ifndef OMEGA_EST_REPLAY_H
#define OMEGA_EST_REPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "est_kf.h"
#include "est_types.h"

#define EST_PROTOCOL_COMMIT "f96dc97"
#define EST_RUN_A_ID "20260929T020536Z-ad8e1f2ea4e4-silicon"
#define EST_RUN_B_ID "3e9e53be3358"
#define EST_RUN_A_SHA "6df4a2b079482b79c42da84da5627b6a8fb9c7d3ce885aac53437b0199d517db"
#define EST_BURN_IN 30u
#define EST_NQ 19
#define EST_NR 25
#define EST_SOURCE_TEXT "R15.machine-state.thermal_mc[0]"

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
void est_file_free(est_file *f);
/* Hex SHA-256 of a whole file on disk (0 on success). */
int est_sha_file_hex(const char *path, char out[65]);
void est_hex(const uint8_t *d, size_t n, char *out);

/* Parsers. Return 0 on success, nonzero when the line does not parse. */
int est_parse_t(const char *line, size_t len, int64_t *t_ns);
int est_parse_thermal0(const char *line, size_t len, double *value);
/* "<sec>.<9 digits>" at the start of s (used for marks lines). */
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

/* Parameter file (text). */
typedef struct {
    char fit_path[1024];
    char fit_sha[65];
    size_t fit_lines;
    double q[2], r[2], ll[2];
} est_params;
int est_params_write(const char *path, const est_params *p);
int est_params_read(const char *path, est_params *p);

#endif
