/* DUAL applicability experiment (ARCH-0031 / ADR 0031, offline evidence lane):
 * pre-registered parameters. Every constant below is part of the experiment
 * identity: ap_params_digest() hashes the canonical text of these values
 * (ap_params_text) and the digest is written into every results file. A
 * change to any value is a new experiment version, never a rescoring.
 *
 * This is an EXPERIMENT, not a production gate and not a production
 * classifier. Nothing here is linked by src/runtime/. */
#ifndef DUAL_APPLIC_PARAMS_H
#define DUAL_APPLIC_PARAMS_H

#define AP_EXPERIMENT_NAME "DUAL-APPLIC-V1"

/* --- time --- */
#define AP_WINDOW 20u            /* steps per aggregate window */
#define AP_WINDOWS 100u          /* windows per run */
#define AP_STEPS (AP_WINDOW * AP_WINDOWS)
#define AP_WARMUP_WINDOWS 5u     /* windows excluded from error scoring */

/* --- capacity --- */
#define AP_SLOTS 32u             /* slot capacity (service lanes) */
#define AP_MEMORY 1024u          /* memory budget in units */
#define AP_MAX_QUEUE 256u        /* queue capacity; overflow is REJECTED (ledgered) */
#define AP_CLASSES 4u            /* queue classes, 0 = highest priority */
#define AP_MAX_ITEMS 32768u      /* bounded pool per run; exhaustion fails the run */

/* --- homogeneous item family (workloads a, c, d, e, f, first half of g) --- */
#define AP_HOM_DEMAND_MIN 11u    /* service demand in steps, uniform [MIN, MAX] */
#define AP_HOM_DEMAND_MAX 13u
#define AP_HOM_MEM_MIN 16u
#define AP_HOM_MEM_MAX 24u
#define AP_HOM_SLOTS 1u

/* --- heterogeneous tool-heavy item family (workload b, second half of g) ---
 * demand = clamp(round(exp(LN_MEDIAN + SIGMA * z)), 1, DEMAND_CAP), z ~ N(0,1) */
#define AP_HET_LN_MEDIAN 1.7917594692280550  /* ln 6 */
#define AP_HET_SIGMA 1.2
#define AP_HET_DEMAND_CAP 400u
#define AP_HET_SLOTS_P1 0.70     /* P(slots = 1); P(2) = P2 - P1; else 4 */
#define AP_HET_SLOTS_P2 0.90
#define AP_HET_MEM_BASE 8u       /* mem = BASE + round(SPAN * u^3) */
#define AP_HET_MEM_SPAN 120.0
#define AP_HET_CLASS_P0 0.55     /* cumulative class probabilities */
#define AP_HET_CLASS_P1 0.80
#define AP_HET_CLASS_P2 0.95

/* --- arrival processes (items per step, Poisson unless stated) --- */
#define AP_RATE_HOM 2.2          /* (a), (f) before the drop, (g) first half */
#define AP_RATE_HET 1.2          /* (b), (g) second half */
#define AP_STEP_RATE_LOW 1.0     /* (c) before AP_SWITCH_STEP */
#define AP_STEP_RATE_HIGH 3.0    /* (c) after */
#define AP_SQUARE_RATE_LOW 0.8   /* (d) alternating every AP_SQUARE_HALF_PERIOD steps */
#define AP_SQUARE_RATE_HIGH 3.2
#define AP_SQUARE_HALF_PERIOD 100u
#define AP_BURST_P 0.08          /* (e) per-step burst probability */
#define AP_BURST_MIN 15u         /* (e) burst size uniform [MIN, MAX] */
#define AP_BURST_MAX 35u
#define AP_BURST_BACKGROUND 0.4  /* (e) Poisson rate on non-burst steps */
#define AP_CAP_DROP_SLOTS 20u    /* (f) slot capacity after AP_SWITCH_STEP */
#define AP_SWITCH_STEP (AP_STEPS / 2u)

/* --- (h) heterogeneity sweep: sigma = MIN + (MAX - MIN) * seed_index / (AP_SEEDS - 1);
 * rate = LOAD * slots / (exp(LN_MEDIAN + sigma^2 / 2) * E[slots]) so offered load stays at LOAD --- */
#define AP_SWEEP_SIGMA_MIN 0.0
#define AP_SWEEP_SIGMA_MAX 1.5
#define AP_SWEEP_LOAD 0.80
#define AP_HET_MEAN_SLOTS 1.7    /* 0.7*1 + 0.2*2 + 0.1*4 */

/* --- aggregate predictor priors (used until the first completion) --- */
#define AP_PRIOR_DEMAND 12.0
#define AP_PRIOR_SLOTS 1.0
#define AP_PRIOR_MEM 20.0
#define AP_BLOCKED_RAMP_START 0.75   /* predicted blocked fraction ramps 0 -> 1 as predicted occupancy goes RAMP_START -> 1 */

/* --- analysis --- */
#define AP_SEEDS 24u             /* seeds per workload (>= 20 pre-registered) */
#define AP_SEED_BASE 0x4455414C2D415031ull  /* "DUAL-AP1" */
#define AP_TOP_SHARE_FRACTION 0.05  /* top-k concentration, k = ceil(5% of items) */
#define AP_GRID_CV_COUNT 3u
#define AP_GRID_TOP_COUNT 3u
#define AP_GRID_CV {0.4, 0.8, 1.2}
#define AP_GRID_TOP {0.10, 0.20, 0.30}
#define AP_GRID_ARRIVAL_CV 0.5
#define AP_BROKEN_CONST 0.5      /* negative control: constant "metric" */

#endif
