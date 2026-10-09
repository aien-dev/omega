/* Internal clock POVM: covariant discrete clock states and the normalization
 * residual. The phase of each component is reduced exactly (rational, mod one
 * turn) before cos/sin, so no accumulated angle drift and no lookup table. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

#define AT0_TWO_PI 6.283185307179586476925286766559

/* exp(-2 pi i E_j k tau) with the turn count reduced exactly in 128-bit integers */
static at0_status clock_phase(const at0_case *c, int j, int k, double complex *out)
{
    if (j < 0 || j >= c->clock_dim || k < 0 || k >= c->label_count) return AT0_ERR_ARGUMENT;
    at0_rat e = c->clock_energies[j], tau = c->povm_tau_turns;
    at0_i128 n = (at0_i128)e.n * k * tau.n;          /* |n| <= 2^20 * 2^8 * 2^20 = 2^48 */
    at0_i128 d = (at0_i128)e.d * tau.d;              /* d <= 2^40 */
    /* x = n/d ; reduce to [-1/2, 1/2): x - round_half_up(x) */
    at0_i128 twice = 2 * n + d;
    at0_i128 q = twice / (2 * d);
    if (twice < 0 && twice % (2 * d) != 0) q -= 1;
    at0_i128 rn = n - q * d;                          /* |rn| <= d */
    double x = (double)(long long)rn / (double)(long long)d;   /* both exact in binary64 */
    double theta = -AT0_TWO_PI * x;
    double cs = cos(theta), sn = sin(theta);
    if (!isfinite(cs) || !isfinite(sn)) return AT0_ERR_NONFINITE;
    *out = cs + I * sn;
    return AT0_OK;
}

at0_status at0_povm_clock_state(const at0_case *c, int k, at0_clock_vec *out)
{
    if (!c || !out) return AT0_ERR_ARGUMENT;
    if (c->clock_dim < AT0_CLOCK_DIM_MIN || c->clock_dim > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    if (k < 0 || k >= c->label_count) return AT0_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->clock_dim = c->clock_dim;
    double scale = 1.0 / sqrt((double)c->clock_dim);
    for (int j = 0; j < c->clock_dim; j++) {
        double complex ph;
        at0_status st = clock_phase(c, j, k, &ph);
        if (st != AT0_OK) return st;
        out->v[j] = scale * ph;
    }
    return AT0_OK;
}

at0_status at0_povm_residual(const at0_case *c, double *out)
{
    if (!c || !out) return AT0_ERR_ARGUMENT;
    int n = c->clock_dim;
    if (n < AT0_CLOCK_DIM_MIN || n > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    double w = at0_rat_to_double(c->povm_weight);
    double complex s[AT0_CLOCK_DIM_MAX][AT0_CLOCK_DIM_MAX];
    memset(s, 0, sizeof s);
    for (int k = 0; k < c->label_count; k++) {
        at0_clock_vec t;
        at0_status st = at0_povm_clock_state(c, k, &t);
        if (st != AT0_OK) return st;
        for (int j = 0; j < n; j++)
            for (int l = 0; l < n; l++)
                s[j][l] += w * t.v[j] * conj(t.v[l]);
    }
    double acc = 0.0;
    for (int j = 0; j < n; j++) {
        for (int l = 0; l < n; l++) {
            double complex d = s[j][l] - (j == l ? 1.0 : 0.0);
            acc += creal(d) * creal(d) + cimag(d) * cimag(d);
        }
    }
    double r = sqrt(acc);
    if (!isfinite(r)) return AT0_ERR_NONFINITE;
    *out = r;
    return AT0_OK;
}
