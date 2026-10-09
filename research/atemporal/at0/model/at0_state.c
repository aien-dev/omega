/* Quantum state representation: a vector in C^N (x) C^2, clock factor first.
 * Every accessor checks its indices; nothing here is global. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

static int index_ok(const at0_state *s, int j, int sys)
{
    return s && s->clock_dim >= AT0_CLOCK_DIM_MIN && s->clock_dim <= AT0_CLOCK_DIM_MAX &&
           j >= 0 && j < s->clock_dim && sys >= 0 && sys < AT0_SYSTEM_DIM;
}

at0_status at0_state_init(at0_state *s, int clock_dim)
{
    if (!s || clock_dim < AT0_CLOCK_DIM_MIN || clock_dim > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    memset(s, 0, sizeof *s);
    s->clock_dim = clock_dim;
    return AT0_OK;
}

at0_status at0_state_get(const at0_state *s, int j, int sys, double complex *out)
{
    if (!out || !index_ok(s, j, sys)) return AT0_ERR_ARGUMENT;
    *out = s->amp[j * AT0_SYSTEM_DIM + sys];
    return AT0_OK;
}

at0_status at0_state_set(at0_state *s, int j, int sys, double complex val)
{
    if (!index_ok(s, j, sys)) return AT0_ERR_ARGUMENT;
    s->amp[j * AT0_SYSTEM_DIM + sys] = val;
    return AT0_OK;
}

at0_status at0_state_norm2(const at0_state *s, double *out)
{
    if (!s || !out || s->clock_dim < AT0_CLOCK_DIM_MIN || s->clock_dim > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    double acc = 0.0;
    int n = s->clock_dim * AT0_SYSTEM_DIM;
    for (int i = 0; i < n; i++) {
        double re = creal(s->amp[i]), im = cimag(s->amp[i]);
        acc += re * re + im * im;
    }
    if (!isfinite(acc)) return AT0_ERR_NONFINITE;
    *out = acc;
    return AT0_OK;
}

at0_status at0_state_inner(const at0_state *a, const at0_state *b, double complex *out)
{
    if (!a || !b || !out || a->clock_dim != b->clock_dim) return AT0_ERR_ARGUMENT;
    if (a->clock_dim < AT0_CLOCK_DIM_MIN || a->clock_dim > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    double complex acc = 0.0;
    int n = a->clock_dim * AT0_SYSTEM_DIM;
    for (int i = 0; i < n; i++) acc += conj(a->amp[i]) * b->amp[i];
    if (!isfinite(creal(acc)) || !isfinite(cimag(acc))) return AT0_ERR_NONFINITE;
    *out = acc;
    return AT0_OK;
}

at0_status at0_state_scale(at0_state *s, double complex z)
{
    if (!s || s->clock_dim < AT0_CLOCK_DIM_MIN || s->clock_dim > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    int n = s->clock_dim * AT0_SYSTEM_DIM;
    for (int i = 0; i < n; i++) s->amp[i] *= z;
    return at0_state_check_finite(s);
}

at0_status at0_state_check_finite(const at0_state *s)
{
    if (!s || s->clock_dim < AT0_CLOCK_DIM_MIN || s->clock_dim > AT0_CLOCK_DIM_MAX) return AT0_ERR_ARGUMENT;
    int n = s->clock_dim * AT0_SYSTEM_DIM;
    for (int i = 0; i < n; i++)
        if (!isfinite(creal(s->amp[i])) || !isfinite(cimag(s->amp[i]))) return AT0_ERR_NONFINITE;
    return AT0_OK;
}
