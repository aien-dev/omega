/* TURING quantized continuous scoring, method id "qint.v1", plus L(M) pieces.
 * Companion to ty_math: code lengths are binary64 bits, converted once per
 * point to int64 micro-bits (ub) and summed with ty_add. New file; ty_math is
 * only read. Wave 1 scores the Gaussian family (0) only: Student-t (1) and
 * mixture (2) predictions are refused with TYQ_FAIL_PROTOCOL. */
#ifndef TURING_TY_QCONT_H
#define TURING_TY_QCONT_H

#include <stddef.h>
#include <stdint.h>

#define TYQ_QUANTIZER_ID "qint.v1"
#define TYQ_DELTA        0x1p-20
#define TYQ_SD_MIN       0x1p-10
#define TYQ_KMAX         8
#define TYQ_NORM_TOL     1e-9
#define TYQ_FAM_GAUSS    0u
#define TYQ_FAM_STUDENT  1u   /* refused in v1 */
#define TYQ_FAM_MIX      2u   /* refused in wave 1 (family 0 only) */

/* Largest |k| accepted: the generator refuses |x| >= 2^31, so |k| < 2^51. */
#define TYQ_K_LIMIT      (INT64_C(1) << 51)

/* Refusal codes, disjoint from TY_E_*. */
enum {
    TYQ_OK = 0,
    TYQ_FAIL_PROTOCOL = -101, TYQ_FAIL_FLOOR = -102, TYQ_FAIL_LEAK = -103,
    TYQ_FAIL_DIGEST = -104, TYQ_FAIL_PROFILE = -105, TYQ_FAIL_BASELINE = -106,
    TYQ_E_ARG = -107, TYQ_E_IO = -108
};
const char *tyq_status_name(int s);

typedef struct { double pi, loc, scale; } tyq_comp;
typedef struct {
    uint32_t index, family;
    double loc, scale;               /* family 0 */
    uint32_t K;                      /* family 2 only (unused in wave 1) */
    tyq_comp comp[TYQ_KMAX];
} tyq_pred;

double tyq_lnsinhc(double a);        /* piecewise, a >= 0 */
/* -log2 p_M(k) for one symbol under qint.v1; sd floor applied and reported. */
int ty_qcont_bits(const tyq_pred *p, int64_t k, double *bits, int *floor_hit);
/* llrint(1e6 * bits) under FE_TONEAREST; TYQ_FAIL_PROTOCOL if bits is
 * non-finite or 1e6*bits > 9.0e18. */
int ty_qcont_point_ub(const tyq_pred *p, int64_t k, int64_t *ub, int *floor_hit);
/* p and k are arrays of n held-out points; sums with ty_add; overflow ->
 * TYQ_FAIL_PROTOCOL. floor_hits is the count of raised scales. */
int ty_qcont_sum_ub(const tyq_pred *p, const int64_t *k, size_t n,
                    int64_t *ld_ub, uint64_t *floor_hits);
/* bytes_ub = 8e6 * program_bytes exactly; rider_ub = llrint(1e6 * k*0.5*log2(n)),
 * k = 0 gives 0 (n may then be 0); k > 0 needs n >= 1. */
int ty_qcont_bytes_ub(uint32_t program_bytes, int64_t *bytes_ub);
int ty_qcont_rider_ub(uint32_t k_params, uint32_t n_prefix_incr, int64_t *rider_ub);
/* TYQ_FAIL_FLOOR iff floor_hits * 1000 > scored_points */
int ty_qcont_floor_check(uint64_t floor_hits, uint64_t scored_points);

#endif
