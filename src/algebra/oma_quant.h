/* Omega mixed algebra: float -> ternary quantization.
 * APPROXIMATE transform, not an exact realization: w is replaced by q*scale
 * with q in {-1,0,1}. Use oma_quant_rel_l2 to measure what was lost. */
#ifndef OMA_QUANT_H
#define OMA_QUANT_H

#include "algebra/oma_trit.h"

/* absmean (BitNet b1.58 style):
 *   scale = mean(|w_i|)   (accumulated in double, stored as float)
 *   q_i   = clamp(round(w_i / scale), -1, 1), round = half away from zero
 * All-zero input: scale = 0, q = 0. Non-finite input -> OMA_E_ARG.
 * No epsilon is added (BitNet adds a tiny eps; here zero scale is special-cased). */
int oma_quant_absmean(const float *w, size_t n, int8_t *q, float *scale);

/* Relative L2 error ||w - q*scale||_2 / ||w||_2 (0 when ||w|| = 0 and the
 * reconstruction is also 0). Computed in double. */
int oma_quant_rel_l2(const float *w, const int8_t *q, float scale, size_t n, double *err);

#endif /* OMA_QUANT_H */
