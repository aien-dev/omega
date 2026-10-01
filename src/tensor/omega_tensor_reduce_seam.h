#ifndef OMEGA_TENSOR_REDUCE_SEAM_H
#define OMEGA_TENSOR_REDUCE_SEAM_H

/*
 * The one place where tensor reductions meet the E1 reduction contract.
 *
 * The order is OMEGA_TENSOR_REDUCE_DECLARED_ORDER (omega_tensor.h), the same
 * string as OMEGA_REDUCE_DECLARED_ORDER in src/omega_numeric_reduce.h of the
 * E1 WP-D reductions (omega PR #134, docs/numeric/E1_REDUCTION_CONTRACT.md):
 *
 *   level 0 values are x[0..n). One level turns len values into ceil(len/32)
 *   values: value j of the next level is the 32-lane warp tree (lane i
 *   combines with lane i+d for d = 16, 8, 4, 2, 1, lower lane on the left)
 *   over values [32j, 32j+32) of this level, positions >= len filled with the
 *   identity (SUM/MEAN: -0.0, MAX/MIN: quiet NaN). At least one level is
 *   applied; levels repeat until one value is left. MEAN = omega_math_div(SUM,
 *   (float)n), 1 <= n <= 2^24.
 *
 * Two builds of the same seam:
 *   default                     local implementation of that order; every
 *                               combine is an E1 CPU-tier op
 *                               (omega_numeric_cpu_realize FADD / FMNMX / DIV).
 *   -DOMEGA_TENSOR_USE_E1_REDUCE calls omega_reduce_cpu from
 *                               src/omega_numeric_reduce.h (available once
 *                               PR #134 is on main); nothing else changes.
 * Either build must give the same bits; the test checks the order string and
 * the bits against an independent reference of the definition.
 */

#include <stddef.h>
#include "omega_tensor.h"

/* Returns 0 or an OMEGA_NUMERIC_* code. n >= 1. */
int omega_tensor_seam_reduce_cpu(OmegaTensorReduceOp op, const float *x, size_t n, float *out);
/* Which build is linked: "LOCAL_E1_CPU_TIER" or "E1_WP_D_OMEGA_REDUCE_CPU". */
const char *omega_tensor_seam_reduce_source(void);

#endif /* OMEGA_TENSOR_REDUCE_SEAM_H */
