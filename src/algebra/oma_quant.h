/* Omega mixed algebra: float -> ternary quantization.
 * APPROXIMATE transform, not an exact realization: w is replaced by q*scale
 * with q in {-1,0,1}. Use oma_quant_rel_l2 to measure what was lost. */
#ifndef OMA_QUANT_H
#define OMA_QUANT_H

#include "algebra/oma_trit.h"

/* absmean (BitNet b1.58 style):
 *   mean  = sum|w_i| / n  (sum and division in double)
 *   scale = (float)mean
 *   q_i   = clamp(round(w_i / scale), -1, 1), round = half away from zero,
 *           division in double (the reference). Equivalently:
 *           q_i = sign(w_i) if |w_i| >= scale/2 (exact real comparison),
 *           else 0. A correctly rounded float32 division gives the same q;
 *           approximate reciprocals or flush-to-zero modes may not.
 * All-zero (or empty) input: OMA_OK, scale = 0, q = 0.
 * Non-zero input with mean < FLT_MIN (scale would be subnormal or 0):
 *   OMA_E_UNDERFLOW, outputs untouched. Hence a successful call always
 *   returns scale = 0 (input all zero) or a normal float.
 * Non-finite input -> OMA_E_ARG, outputs untouched.
 * No epsilon is added (BitNet adds a tiny eps; here zero scale is special-cased). */
int oma_quant_absmean(const float *w, size_t n, int8_t *q, float *scale);

/* Relative L2 error ||w - q*scale||_2 / ||w||_2, computed in double.
 * OMA_E_ARG: non-finite w_i, scale NaN/Inf/negative, or ||w|| = 0 with a
 * non-zero reconstruction (relative error undefined). ||w|| = 0 with zero
 * reconstruction -> 0. scale = 0 is accepted (absmean's all-zero output).
 * q_i outside {-1,0,1} -> OMA_E_INVALID_TRIT. On OMA_OK, *err is finite. */
int oma_quant_rel_l2(const float *w, const int8_t *q, float scale, size_t n, double *err);

#endif /* OMA_QUANT_H */
