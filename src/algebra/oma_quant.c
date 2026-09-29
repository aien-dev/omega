#include "algebra/oma_quant.h"

#include <math.h>

int oma_quant_absmean(const float *w, size_t n, int8_t *q, float *scale) {
    if (!scale || (n && (!w || !q))) return OMA_E_ARG;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(w[i])) return OMA_E_ARG;
        sum += fabs((double)w[i]);
    }
    if (n == 0 || sum == 0.0) {
        for (size_t i = 0; i < n; i++) q[i] = 0;
        *scale = 0.0f;
        return OMA_OK;
    }
    float s = (float)(sum / (double)n);
    if (!(s > 0.0f)) { /* mean underflowed float: treat as zero input */
        for (size_t i = 0; i < n; i++) q[i] = 0;
        *scale = 0.0f;
        return OMA_OK;
    }
    for (size_t i = 0; i < n; i++) {
        double r = round((double)w[i] / (double)s);
        q[i] = (int8_t)(r > 1.0 ? 1 : (r < -1.0 ? -1 : (int)r));
    }
    *scale = s;
    return OMA_OK;
}

int oma_quant_rel_l2(const float *w, const int8_t *q, float scale, size_t n, double *err) {
    if (!err || (n && (!w || !q))) return OMA_E_ARG;
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (q[i] < -1 || q[i] > 1) return OMA_E_INVALID_TRIT;
        double d = (double)w[i] - (double)q[i] * (double)scale;
        num += d * d;
        den += (double)w[i] * (double)w[i];
    }
    if (den == 0.0) {
        *err = num == 0.0 ? 0.0 : INFINITY;
        return OMA_OK;
    }
    *err = sqrt(num / den);
    return OMA_OK;
}
