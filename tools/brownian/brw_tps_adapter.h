/* TPS1 binariser (lane T, phase 1). Family-free: it sees only a Normal
 * predictive (mu, sd) and an observed qint.v1 bin index b. The observation is
 * turned into a short sequence of binary steps:
 *   step 0      zero / nonzero offset from the centre bin
 *   step 1      sign (only if nonzero)
 *   then        unary over Elias-gamma classes [2^j, 2^(j+1)) of |offset|
 *   then        j binary refinement bits inside the class, most significant first
 * Step probabilities are ratios of exact Normal set masses, held as long-double
 * log masses (needs binary128 long double, as on aarch64). The chain of ratios
 * telescopes to the exact Normal mass of bin b.
 *
 * Note on qint.v1: ty_qcont_bits is a closed-form approximation of the bin
 * mass (linearised exponent, no renormalisation). Its sum over bins is
 * about 1 + (delta/sd)^2/24, so a partition-based binariser can match it to
 * 1e-9 bits only where (delta/sd)^2/24 is small (sd of a few times sd_min and
 * up). The stream check gates hard on the exact Normal mass and gates on the
 * qint.v1 codelength with an analytic per-observation bound; both maxima are
 * reported. No coder is called here; the phase-2 hookup fills brw_tps_coder_fn.
 * Range: |mu| < 2^31 and |b| < 2^51 (qint.v1 limit), so |b - centre| < 2^52 bins
 * (2^42 sd at sd_min); 51 classes cover it, anything beyond is refused with a
 * negative code. All masses are log-domain, so z of thousands of sd is fine.
 * Range: |mu| < 2^31 and |b| < 2^51 (qint.v1 limit), so |b - centre| < 2^52 bins
 * (2^42 sd at sd_min); 51 classes cover it, anything beyond is refused with a
 * negative code. All masses are log-domain, so z of thousands of sd is fine. */
#ifndef BRW_TPS_ADAPTER_H
#define BRW_TPS_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

enum {
    BRW_TPS_OK = 0,
    BRW_TPS_E_ARG = -201,    /* null pointer or bad argument */
    BRW_TPS_E_RANGE = -202,  /* NaN, sd <= 0, |mu| or bin index out of range, qint.v1 refusal */
    BRW_TPS_E_CAP = -203,    /* step buffer too small */
    BRW_TPS_E_PROOF = -204,  /* codelength proof violated (a STOP) */
    BRW_TPS_E_NUM = -205,    /* numeric failure inside the mass code */
    BRW_TPS_E_CODER = -206   /* the coder callback refused */
};

#define BRW_TPS_MAX_STEPS 112u
#define BRW_TPS_EXACT_TOL 1e-9

typedef struct {
    double p1;            /* probability that bit == 1 */
    int bit;
    long double log2p_taken;   /* log2 probability of the branch taken, from log masses */
    long double log2p_other;   /* log2 probability of the branch not taken */
} brw_tps_step;

/* Binarise one observation. steps has room for cap entries (use
 * BRW_TPS_MAX_STEPS). Outputs: *nsteps, *bits_steps (sum of -log2 p_taken),
 * *bits_exact (-log2 of the exact Normal mass of bin b). Either bits pointer
 * may be NULL. */
int brw_tps_binarise(double mu, double sd, int64_t b, size_t cap,
                     brw_tps_step *steps, size_t *nsteps,
                     long double *bits_steps, long double *bits_exact);

/* Sanity check of one observation's steps: each step's two branch
 * probabilities sum to 1 (1e-12) and p1 agrees with log2p. Returns 0 or
 * BRW_TPS_E_PROOF. */
int brw_tps_check_steps(const brw_tps_step *steps, size_t n);

/* Whole-stream proof. For every observation: steps are consistent, and
 * |sum(-log2 p_step) - exact bin codelength| <= BRW_TPS_EXACT_TOL, and
 * |sum - ty_qcont_bits| <= -log2(1 - (delta/sd)^2/8) + 1e-9 (sd floored at
 * sd_min), the analytic bound for the unnormalised qint.v1 masses. Any
 * violation returns BRW_TPS_E_PROOF and sets *bad_index; a refusal returns
 * its error code. Outputs: max exact deviation, max qint deviation, and the
 * max of (qint deviation - bound), which is <= 1e-9 on success. */
int brw_tps_check_stream(const double *mu, const double *sd, const int64_t *b,
                         size_t n, size_t *bad_index,
                         double *max_dev_exact, double *max_dev_qint,
                         double *max_qint_excess);

/* [f0, f1] u16 frequency row for a step: f0 + f1 == 65536, each >= 1. */
int brw_tps_p1_to_freq(double p1, uint16_t freq[2]);

/* Filled by the phase-2 coder hookup. Return 0 on success. */
typedef int (*brw_tps_coder_fn)(void *ctx, const uint16_t freq[2], int bit);

/* Convert each step to a frequency row and hand it to the callback. */
int brw_tps_emit(const brw_tps_step *steps, size_t n, brw_tps_coder_fn fn, void *ctx);

#endif
