/* Omega mixed algebra: phase-domain Z3 digital twin (spec/mixed-algebra-phase-twin.md).
 *
 * A software model of a physical realization of Z3 = {0,1,2} carried as the
 * phase of a sampled tone: value k is emitted as A cos(2 pi n / C + 2 pi k / 3),
 * f_c = f_s / C. Measurement is an I/Q correlation at the carrier bin, decisions
 * use +/-60 degree cells, and a reference-frame calibration removes a
 * per-channel fixed phase offset and a slow drift.
 *
 * Z3 addition is realized as phasor multiplication (theory section 6.2) in
 * three declared ways:
 *   PT_ADD_CORR_PRODUCT  two channels, each correlated, correlator outputs
 *                        multiplied (the loopback in the research note);
 *   PT_ADD_MIXER         analog mixer: sample-wise product of the two tones,
 *                        correlated at the 2 f_c bin;
 *   PT_ADD_NCO           32-bit NCO phase-word accumulation, one tone.
 * Z3 multiplication by a KNOWN constant b (theory section 6.4) is realized as
 * constant source (b = 0), identity (b = 1) or frequency doubling (b = 2).
 * Z3 multiplication of two unknown phases has no phasor realization in this
 * model and is not provided.
 *
 * SIMULATED_DEVELOPMENT only: this is a model, never physical evidence.
 * C11, no dependencies beyond libm. Deterministic for a given seed. */
#ifndef PHASE_TWIN_H
#define PHASE_TWIN_H

#include <stdint.h>

#define PT_PI 3.14159265358979323846

enum {
    PT_OK = 0,
    PT_E_ARG = -1,      /* NULL, bad parameter, value outside {0,1,2} */
    PT_E_NOMEM = -2
};

/* ---- deterministic RNG: xoshiro256** seeded by splitmix64 ---- */
typedef struct {
    uint64_t s[4];
    int has_spare;
    double spare;
} pt_rng;

void pt_rng_seed(pt_rng *r, uint64_t seed);
uint64_t pt_rng_next(pt_rng *r);
double pt_rng_uniform(pt_rng *r);   /* [0, 1) with 53 bits */
double pt_rng_gauss(pt_rng *r);     /* N(0,1), Marsaglia polar method */

/* ---- configuration ---- */
typedef enum {
    PT_ADD_CORR_PRODUCT = 1,
    PT_ADD_MIXER = 2,
    PT_ADD_NCO = 3
} pt_add_mode;

typedef struct {
    double fs_hz;             /* sample rate (declared; sets the time base) */
    uint32_t cycle_samples;   /* samples per carrier cycle: f_c = fs / cycle_samples */
    uint32_t n_samples;       /* correlation window N; multiple of cycle_samples */
    double amplitude;         /* tone peak, fraction of converter full scale (+/-1) */
    int dac_bits;             /* 0 = ideal (no quantization, no clipping) */
    int adc_bits;             /* 0 = ideal */
    double noise_sigma;       /* AWGN std per sample per channel, full-scale units */
    double jitter_deg;        /* rms Gaussian phase jitter per channel per frame */
    double offset_deg[2];     /* fixed per-channel path phase */
    double drift_deg_per_s[2];/* linear per-channel phase drift */
    double frame_s;           /* simulated time per emitted frame */
} pt_channel_params;

typedef struct {
    int enabled;
    uint32_t interval_trials; /* recalibrate every this many operations */
    uint32_t reps;            /* reference frames averaged per calibration step */
    double check_tol_deg;     /* k = 1 check: 120 +/- tol after correction */
} pt_cal_params;

typedef struct {
    double erasure_frac;      /* magnitude below frac x reference: erasure (0 = off) */
    double reject_deg;        /* output phase farther than this from a center: reject (0 = off) */
} pt_flag_params;

typedef struct {
    pt_channel_params ch;
    pt_cal_params cal;
    pt_flag_params flags;
    pt_add_mode mode;
} pt_config;

/* Per-sample sigma giving correlator-output symbol SNR rho = A^2 / N0
 * (research note: rho = N SNR_1 / 2, SNR_1 = (A^2/2)/sigma^2). */
double pt_sigma_for_rho(double amplitude, uint32_t n_samples, double rho_linear);

/* ---- the twin ---- */
enum {
    PT_FLAG_ERASURE = 1u,
    PT_FLAG_REJECT = 2u,
    PT_FLAG_CALFAULT = 4u
};

typedef struct {
    int decoded;      /* decoded Z3 output */
    int decoded_a;    /* CORR_PRODUCT only: channel a read alone, else -1 */
    int decoded_b;    /* CORR_PRODUCT only: channel b read alone, else -1 */
    unsigned flags;   /* PT_FLAG_*; any flag fails the trial */
    double phase_deg; /* output phase after calibration, (-180, 180] */
} pt_result;

typedef struct {
    pt_config cfg;
    pt_rng rng;
    double t_s;
    uint64_t frames;
    double *buf_a, *buf_b, *buf_m;
    double *cos_tab, *sin_tab;
    /* calibration state */
    int cal_valid;
    int cal_fault;
    uint64_t ops_since_cal;
    double cal_deg[2];      /* per-channel offset estimate (CORR_PRODUCT, NCO: [0]) */
    double cal_mag[2];      /* per-channel reference magnitude */
    double cal_mix_deg;     /* MIXER: combined offset estimate */
    double cal_mix_mag;
    double cal_dbl_deg;     /* doubler path offset estimate (mul by 2) */
    double cal_dbl_mag;
    int want_dbl;           /* calibrate the doubler path too (set by mul_const) */
    uint64_t cal_count;
    uint64_t cal_faults;
} pt_twin;

int pt_twin_init(pt_twin *tw, const pt_config *cfg, uint64_t seed);
void pt_twin_free(pt_twin *tw);

/* One Z3 addition through the physical model. Handles calibration schedule. */
int pt_twin_add(pt_twin *tw, int a, int b, pt_result *out);
/* One Z3 multiplication by a known digital constant b (0, 1, 2). */
int pt_twin_mul_const(pt_twin *tw, int a, int b, pt_result *out);

/* Decision: k = mod(round(theta / (2 pi / 3)), 3); dist_deg = |distance to center|. */
int pt_decode(double theta_rad, double *dist_deg);

/* NCO phase word for one third of a turn: floor(2^32 / 3). */
#define PT_NCO_THIRD 1431655765u

/* ---- statistics and analytic references ---- */
typedef struct { double lo, hi; } pt_interval;
pt_interval pt_wilson(uint64_t errors, uint64_t n, double z);
/* E1 bound (ADR 0019 section 8): zero failures in n => p <= -ln(alpha)/n. */
double pt_e1_bound(uint64_t n, double alpha);
/* Exact coherent 3-PSK symbol error for one phasor (Craig's integral). */
double pt_ref_single_craig(double rho);
/* Exact error of the add: phase of the product of two independent noisy
 * phasors, each at symbol SNR rho, by numerical circular convolution of the
 * exact PSK phase density. */
double pt_ref_add_exact(double rho, int grid);
/* Add under pure Gaussian phase jitter s_deg per input: 2 Q(60 / (sqrt2 s)). */
double pt_ref_add_jitter(double s_deg);

#endif /* PHASE_TWIN_H */
