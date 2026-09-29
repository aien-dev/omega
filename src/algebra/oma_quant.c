#include "algebra/oma_quant.h"

#include <float.h>
#include <math.h>

int oma_quant_absmean(const float *w, size_t n, int8_t *q, float *scale) {
    if (!scale || (n && (!w || !q))) return OMA_E_ARG;
    double sum = 0.0; /* exact enough: each |w_i| <= FLT_MAX, no overflow in double */
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(w[i])) return OMA_E_ARG;
        sum += fabs((double)w[i]);
    }
    if (n == 0 || sum == 0.0) { /* all-zero input (or empty) */
        for (size_t i = 0; i < n; i++) q[i] = 0;
        *scale = 0.0f;
        return OMA_OK;
    }
    double mean = sum / (double)n;
    /* Non-zero input whose mean is not a normal float: a subnormal or zero
     * scale would lose the input silently and break re-quantization, so it is
     * an explicit error. Outputs untouched. */
    if (mean < (double)FLT_MIN) return OMA_E_UNDERFLOW;
    float s = (float)mean; /* >= FLT_MIN, finite (mean <= FLT_MAX) */
    /* Reference division is double. q depends only on whether |w_i| >= s/2;
     * the double quotient decides that exactly (so does a correctly rounded
     * float32 quotient; see spec). */
    for (size_t i = 0; i < n; i++) {
        double r = round((double)w[i] / (double)s);
        q[i] = (int8_t)(r > 1.0 ? 1 : (r < -1.0 ? -1 : (int)r));
    }
    *scale = s;
    return OMA_OK;
}

int oma_quant_rel_l2(const float *w, const int8_t *q, float scale, size_t n, double *err) {
    if (!err || (n && (!w || !q))) return OMA_E_ARG;
    if (!isfinite(scale) || scale < 0.0f) return OMA_E_ARG; /* NaN, Inf, negative */
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(w[i])) return OMA_E_ARG;
        if (q[i] < -1 || q[i] > 1) return OMA_E_INVALID_TRIT;
        double d = (double)w[i] - (double)q[i] * (double)scale;
        num += d * d;
        den += (double)w[i] * (double)w[i];
    }
    if (den == 0.0) {
        /* ||w|| = 0: relative error is 0 if the reconstruction is also 0,
         * otherwise undefined -> error, never +inf with OMA_OK. */
        if (num != 0.0) return OMA_E_ARG;
        *err = 0.0;
        return OMA_OK;
    }
    *err = sqrt(num / den);
    return OMA_OK;
}
