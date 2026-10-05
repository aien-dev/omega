/* DUAL applicability experiment: deterministic discrete workload simulator,
 * aggregate (macroscopic) predictor, heterogeneity measurements, campaign.
 * Plain C11, no I/O, no allocation, no globals (the caller owns ap_sim).
 * Vocabulary: heterogeneity, concentration, regime, aggregate predictor. */
#ifndef DUAL_APPLIC_H
#define DUAL_APPLIC_H

#include <stddef.h>
#include <stdint.h>
#include "applic_params.h"

typedef enum {
    AP_WL_HOMOGENEOUS = 0,   /* (a) many near-identical items */
    AP_WL_HETEROGENEOUS,     /* (b) long-tailed service, few items hold most load */
    AP_WL_STEP,              /* (c) arrival rate steps up mid-run */
    AP_WL_SQUARE,            /* (d) square-wave congestion */
    AP_WL_BURSTY,            /* (e) bursty arrivals */
    AP_WL_CAPACITY_DROP,     /* (f) slot capacity reduced mid-run */
    AP_WL_REGIME_CHANGE,     /* (g) item family a -> b mid-run */
    AP_WL_HETERO_SWEEP,      /* (h) heterogeneous family, spread rises with seed index, constant offered load */
    AP_WL_COUNT
} ap_workload;

typedef struct { uint64_t s; } ap_rng;
uint64_t ap_rng_next(ap_rng *r);
double ap_rng_unit(ap_rng *r);          /* [0, 1) */

typedef struct {
    uint32_t arrival;        /* step of arrival */
    uint32_t demand0;        /* service demand in steps */
    uint32_t remaining;
    uint16_t mem;            /* memory units held while running */
    uint8_t slots;           /* slots held while running */
    uint8_t cls;             /* queue class, 0 = highest priority */
} ap_item;

/* Aggregates observed at the close of one window. The predictor sees only
 * this record (densities, rates, running means), never the item list. */
typedef struct {
    double used_slots_frac, used_mem_frac, queue_density;
    uint32_t arrivals, completions, blocked_steps, rejected;
    uint32_t in_system, queue_len, running, slots_capacity;
    double mean_demand, mean_slots, mean_mem;  /* running means over completions */
} ap_window;

typedef struct {
    ap_item items[AP_MAX_ITEMS];
    uint32_t n_items;
    uint32_t queue[AP_MAX_QUEUE];
    uint32_t queue_len;
    uint32_t running[AP_SLOTS];
    uint32_t running_len;
    uint32_t used_slots, used_mem, slots_capacity;
    uint64_t arrived, completed, rejected, pool_exhausted;
    double sum_demand, sum_slots, sum_mem;
    uint64_t n_completed_stats;
    ap_window windows[AP_WINDOWS];
    uint32_t n_windows;
    ap_rng rng;
    ap_workload wl;
    double het_sigma;        /* log-spread of the heterogeneous family for this run */
    double sweep_rate;       /* arrival rate for (h); 0 elsewhere */
    uint32_t step;
    uint32_t w_arrivals, w_completions, w_blocked, w_rejected;
} ap_sim;

void ap_sim_init(ap_sim *s, ap_workload wl, uint64_t seed);
/* (h): sets het_sigma and the arrival rate that keeps offered load at
 * AP_SWEEP_LOAD for that sigma. Other workloads ignore seed_index here. */
void ap_sim_init_indexed(ap_sim *s, ap_workload wl, uint64_t seed, uint32_t seed_index);
double ap_sweep_sigma(uint32_t seed_index);
int ap_sim_step(ap_sim *s);              /* 0 ok; 1 ledger violation; 2 pool exhausted */
int ap_sim_ledger_ok(const ap_sim *s);   /* arrived == completed + queued + running + rejected */
int ap_sim_run(ap_sim *s);               /* all AP_STEPS; 0 only if every step returned 0 */

typedef struct { double completions, queue_density, blocked; } ap_prediction;
ap_prediction ap_predict(const ap_window *w);   /* next window from this window's aggregates */

typedef struct {
    double completions_nrmse;   /* RMSE / mean actual completions per window */
    double queue_rmse;          /* queue density is already in [0,1] */
    double blocked_rmse;        /* blocked fraction is already in [0,1] */
    double composite;           /* mean of the three */
    uint32_t windows;           /* windows scored */
} ap_error;
ap_error ap_error_real(const ap_sim *s);
/* Same aggregate model fed the NEXT window's true arrival count: isolates
 * aggregation error from arrival-forecast error. Still aggregate-only. */
ap_error ap_error_known_arrivals(const ap_sim *s);
/* Negative control: predict window t+1 from the aggregates of window
 * ((t + offset) mod scored), offset in [1, scored-1]: the labels are shuffled. */
ap_error ap_error_shuffled(const ap_sim *s, uint32_t offset);

typedef struct {
    double cv_demand;        /* coefficient of variation of service demand */
    double top_share;        /* load share of the top ceil(5%) items (load = demand * slots) */
    double max_item_share;   /* load share of the single largest item */
    double class_entropy;    /* normalized Shannon entropy of class mix, [0,1] */
    double arrival_cv;       /* CV of per-window arrival counts (temporal heterogeneity) */
    double broken_const;     /* negative control: constant */
    double broken_noise;     /* negative control: seeded noise independent of the run */
} ap_hetero;
ap_hetero ap_measure(const ap_sim *s, uint32_t *scratch /* AP_MAX_ITEMS */, ap_rng *noise);

typedef struct {
    uint8_t wl;
    uint32_t seed_index;
    uint64_t seed;
    uint64_t arrived, completed, rejected;
    ap_error real, known_arrivals, shuffled;
    ap_hetero h;
} ap_row;

uint64_t ap_seed(ap_workload wl, uint32_t seed_index);
/* Runs AP_WL_COUNT * AP_SEEDS runs into rows (capacity max_rows). scratch is
 * caller-owned (one ap_sim plus AP_MAX_ITEMS uint32). Returns 0 on success. */
int ap_campaign(ap_sim *scratch, uint32_t *scratch_idx, ap_row *rows, size_t max_rows, size_t *n_out);

const char *ap_workload_name(ap_workload wl);
const char *ap_row_header(void);
int ap_row_format(const ap_row *r, char *buf, size_t n);   /* one CSV line, no newline */
void ap_rows_digest(const ap_row *rows, size_t n, uint8_t out[32]);  /* over CSV text */
int ap_params_text(char *buf, size_t n);
void ap_params_digest(uint8_t out[32]);
/* Pearson r over n pairs; cov_out receives the raw covariance. Zero variance
 * in either series yields r = 0 (reported, never NaN). */
double ap_pearson(const double *x, const double *y, size_t n, double *cov_out);

#endif
