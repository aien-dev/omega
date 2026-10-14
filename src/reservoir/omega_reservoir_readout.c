/* Ridge readout and NRMSE; contract in omega_reservoir_readout.h. */
#include "omega_reservoir_readout.h"

#include <string.h>

static int fin64(double x) { return x == x && x - x == 0.0; }

OmegaResStatus omega_res_readout_fit(const double *x, const double *y, uint32_t n_samples, uint32_t n_features,
                                     uint32_t n_outputs, double lambda, OmegaResReadoutWorkspace *ws,
                                     OmegaResReadout *out)
{
    if (!x || !y || !ws || !out) return OMEGA_RES_ERR_NULL;
    if (n_features < 1u || n_features > OMEGA_RES_READOUT_MAX_FEATURES) return OMEGA_RES_ERR_PARAM;
    if (n_outputs < 1u || n_outputs > OMEGA_RES_READOUT_MAX_OUTPUTS) return OMEGA_RES_ERR_PARAM;
    if (n_samples < 1u || !fin64(lambda) || lambda < 0.0) return OMEGA_RES_ERR_PARAM;
    for (size_t i = 0; i < (size_t)n_samples * n_features; i++)
        if (!fin64(x[i])) return OMEGA_RES_ERR_NONFINITE;
    for (size_t i = 0; i < (size_t)n_samples * n_outputs; i++)
        if (!fin64(y[i])) return OMEGA_RES_ERR_NONFINITE;

    const uint32_t p = n_features + 1u; /* design columns: bias then features */
    const uint32_t D = OMEGA_RES_READOUT_DIM, O = OMEGA_RES_READOUT_MAX_OUTPUTS;
    double *G = ws->g, *d = ws->d, *R = ws->rhs;
    /* Lower triangle of G and all of R, each a sum over samples in order. */
    for (uint32_t i = 0; i < p; i++) {
        for (uint32_t j = 0; j <= i; j++) {
            double s = 0.0;
            for (uint32_t t = 0; t < n_samples; t++) {
                const double xi = i ? x[(size_t)t * n_features + i - 1] : 1.0;
                const double xj = j ? x[(size_t)t * n_features + j - 1] : 1.0;
                s = s + xi * xj;
            }
            G[i * D + j] = s;
        }
        if (i) G[i * D + i] = G[i * D + i] + lambda;
        for (uint32_t o = 0; o < n_outputs; o++) {
            double s = 0.0;
            for (uint32_t t = 0; t < n_samples; t++) {
                const double xi = i ? x[(size_t)t * n_features + i - 1] : 1.0;
                s = s + xi * y[(size_t)t * n_outputs + o];
            }
            R[i * O + o] = s;
        }
    }
    /* G = L D L^T, L unit lower triangular, stored below G's diagonal. */
    for (uint32_t j = 0; j < p; j++) {
        double dj = G[j * D + j];
        const double gjj = dj;
        for (uint32_t k = 0; k < j; k++) dj = dj - (G[j * D + k] * G[j * D + k]) * d[k];
        if (!(dj > 1e-13 * gjj) || !fin64(dj)) return OMEGA_RES_ERR_SINGULAR;
        d[j] = dj;
        for (uint32_t i = j + 1; i < p; i++) {
            double s = G[i * D + j];
            for (uint32_t k = 0; k < j; k++) s = s - (G[i * D + k] * G[j * D + k]) * d[k];
            G[i * D + j] = s / dj;
        }
    }
    for (uint32_t o = 0; o < n_outputs; o++) {
        for (uint32_t i = 0; i < p; i++) { /* L z = r */
            double s = R[i * O + o];
            for (uint32_t k = 0; k < i; k++) s = s - G[i * D + k] * R[k * O + o];
            R[i * O + o] = s;
        }
        for (uint32_t i = 0; i < p; i++) R[i * O + o] = R[i * O + o] / d[i];
        for (uint32_t ii = p; ii-- > 0;) { /* L^T w = z */
            double s = R[ii * O + o];
            for (uint32_t k = ii + 1; k < p; k++) s = s - G[k * D + ii] * R[k * O + o];
            R[ii * O + o] = s;
        }
    }
    OmegaResReadout r;
    memset(&r, 0, sizeof r);
    r.n_features = n_features;
    r.n_outputs = n_outputs;
    r.lambda = lambda;
    for (uint32_t i = 0; i < p; i++)
        for (uint32_t o = 0; o < n_outputs; o++) {
            r.w[i * O + o] = R[i * O + o];
            if (!fin64(r.w[i * O + o])) return OMEGA_RES_ERR_NONFINITE;
        }
    *out = r;
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_readout_apply(const OmegaResReadout *r, const double *x, double *y_out)
{
    if (!r || !x || !y_out) return OMEGA_RES_ERR_NULL;
    if (r->n_features < 1u || r->n_features > OMEGA_RES_READOUT_MAX_FEATURES || r->n_outputs < 1u ||
        r->n_outputs > OMEGA_RES_READOUT_MAX_OUTPUTS)
        return OMEGA_RES_ERR_PARAM;
    const uint32_t O = OMEGA_RES_READOUT_MAX_OUTPUTS;
    for (uint32_t f = 0; f < r->n_features; f++)
        if (!fin64(x[f])) return OMEGA_RES_ERR_NONFINITE;
    for (uint32_t o = 0; o < r->n_outputs; o++) {
        double s = r->w[o];
        for (uint32_t f = 0; f < r->n_features; f++) s = s + x[f] * r->w[(1u + f) * O + o];
        if (!fin64(s)) return OMEGA_RES_ERR_NONFINITE;
        y_out[o] = s;
    }
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_nrmse(const double *ref, const double *pred, uint32_t n, double *out)
{
    if (!ref || !pred || !out) return OMEGA_RES_ERR_NULL;
    if (n < 2u) return OMEGA_RES_ERR_PARAM;
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        if (!fin64(ref[i]) || !fin64(pred[i])) return OMEGA_RES_ERR_NONFINITE;
        sum = sum + ref[i];
    }
    const double mean = sum / (double)n;
    double num = 0.0, den = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const double e = ref[i] - pred[i], c = ref[i] - mean;
        num = num + e * e;
        den = den + c * c;
    }
    if (!(den > 0.0) || !fin64(num) || !fin64(den)) return OMEGA_RES_ERR_SINGULAR;
    *out = __builtin_sqrt(num / den);
    return OMEGA_RES_OK;
}
