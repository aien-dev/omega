/* Shared parts of the omega#440 Gate 2 tool (tools/reservoir/res_qualify):
 * pinned archive files, simulation drivers, statistics, the benchmark ridge
 * readout with blocked cross-validation, a deterministic generator and JSON
 * helpers. Tools may allocate and do I/O; the simulator under src/reservoir
 * does neither. Binary64 here unless stated; the simulator is binary32. */
#ifndef RES_COMMON_H
#define RES_COMMON_H

#include "omega_reservoir.h"
#include "omega_reservoir_readout.h"
#include "res_mat.h"

#include <stdint.h>
#include <stdio.h>

/* ---------------------------------------------------------- archive pins */

/* Files of Zenodo 10.5281/zenodo.17296788 (repo.zip sha256 54582293...a15),
 * paths relative to the unpacked project-zenodo-ready_with-paper/. */
typedef struct {
    const char *key;
    const char *path;
    const char *sha256;
} ResPin;

extern const ResPin RES_PINS[];
extern const unsigned RES_PIN_COUNT;
const ResPin *res_pin(const char *key);
/* Loads a pinned .mat, refusing a sha256 mismatch. 0 on success; prints the
 * reason to stderr otherwise. */
int res_load_pinned(const char *root, const char *key, ResMatFile *f);
/* sha256 of a pinned file (any type, never parsed or executed). 0 = match. */
int res_check_pinned(const char *root, const ResPin *p, char got_hex[65]);

/* -------------------------------------------------------------- helpers */

void res_hex(const uint8_t *d, size_t n, char *out); /* 2n chars + NUL */
void res_json_str(FILE *o, const char *s);
/* Writes ", \"membranes\": {...}": the physical-plausibility record of a run. */
void res_mstats_json(FILE *o, const OmegaResMembraneStats *m);
double res_now_s(void); /* CLOCK_MONOTONIC */
const char *res_status_name(OmegaResStatus s);

/* splitmix64 (Steele, Lea, Flood 2014): the benchmark's only randomness. */
typedef struct {
    uint64_t s;
} ResRng;
uint64_t res_rng_next(ResRng *r);
double res_rng_unit(ResRng *r); /* [0, 1), 53 bits */

/* ----------------------------------------------------------- simulation */

typedef struct {
    OmegaResConfig cfg;
    OmegaResState s, next;
    OmegaResWorkspace ws;
    OmegaResMembraneStats mstats; /* accepted states since the last omega_res_membrane_stats_init */
} ResSim;

/* Runs from the state in sim->s (already initialised) and records the node
 * voltages (binary32 widened to binary64) at the integer steps steps[0..n),
 * which must be non-decreasing: v_out[k * node_count + i]. */
OmegaResStatus res_sim_at_steps(ResSim *sim, const OmegaResInput *in, const uint64_t *steps, size_t n,
                                double *v_out);
/* Same at fractional step positions pos[k] (non-decreasing, >= 0): linear
 * interpolation between the states at floor(pos) and floor(pos) + 1. */
OmegaResStatus res_sim_at_positions(ResSim *sim, const OmegaResInput *in, const double *pos, size_t n,
                                    uint64_t last_step, double *v_out);

/* ------------------------------------------------------------- networks */

typedef enum { RES_NET_HEX13 = 13, RES_NET_HEX25 = 25, RES_NET_HEX43 = 43, RES_NET_CHAIN50 = 50 } ResNet;

/* The reference topologies of tests/reservoir/res_fixtures.h (0-based).
 * inputs0: the three Lorenz injection nodes (lattices only, else unset). */
OmegaResStatus res_net_topology(ResNet net, OmegaResTopology *t, uint32_t inputs0[3], uint32_t *ground0);

/* Disconnected ablation of a network: driven nodes 0..nd-1 keep their
 * concentration conc[k]; each gets exactly one membrane, to a grounded
 * droplet of the same concentration (nodes nd.., one per class per 6 driven
 * nodes). A grounded droplet is held at 0 V, so sharing it couples nothing:
 * the reduced Laplacian is diagonal. Other parameters as the reference. */
OmegaResStatus res_net_disconnected(OmegaResReference which, const float *conc, uint32_t nd, OmegaResTopology *t,
                                    OmegaResParams *p);

/* ----------------------------------------------------------- statistics */

double res_mean(const double *x, size_t n, size_t stride);
double res_std_pop(const double *x, size_t n, size_t stride); /* sqrt(mean((x - mean)^2)) */
double res_rms_diff(const double *a, const double *b, size_t n, size_t stride);
double res_max_abs_diff(const double *a, const double *b, size_t n, size_t stride);
/* NRMSE of Text S6 / goodnessOfFit 'NRMSE' on a strided column. */
double res_nrmse(const double *ref, const double *pred, size_t n, size_t stride);
double res_sqrt(double x);

/* NARMA-o targets exactly as HyperParameterTuningOptimized.m:110-142 (zero
 * history, y(c) depends on u(c-1), u(c-o) and y(c-1..c-o)); y has n
 * entries, 0-based y[c] for the reference's 1-based c + 1. */
void res_narma(const double *u, uint32_t n, uint32_t order, double *y);

/* ------------------------------------------- benchmark ridge readout + CV */

/* Ridge with unpenalized bias on features standardized with the training
 * mean and population std (a constant column is left centred at 0, which the
 * penalty keeps nonsingular). lambda is chosen from grid by blocked K-fold
 * cross-validation on the training rows (contiguous folds, edges
 * round(k * n / K)), score = mean over folds of the mean NRMSE over outputs;
 * ties keep the first. Then refit on all training rows. pred_te may be NULL. */
typedef struct {
    double lambda;
    double cv_score;
    OmegaResStatus status;
} ResCvResult;
ResCvResult res_ridge_cv(const double *xtr, const double *ytr, uint32_t ntr, uint32_t nf, uint32_t no,
                         const double *xte, uint32_t nte, const double *grid, uint32_t ngrid, uint32_t folds,
                         double *pred_tr, double *pred_te);

/* Ordinary least squares with intercept by Householder QR (binary64) on
 * unit-norm-equilibrated columns: the method family of MATLAB fitlm.
 * Diagnostic only (NARMA comparison). w has nf + 1 entries, w[0] the
 * intercept. Returns 0, or -1 if rank deficient (|R_jj| <= 1e-14 max |R_ii|). */
int res_ols_qr(const double *x, const double *y, uint32_t n, uint32_t nf, double *w);

#endif /* RES_COMMON_H */
