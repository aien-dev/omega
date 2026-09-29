/* qint.v1 closed-form scorer. See ty_qcont.h. binary64, log space, no
 * probability floor, no renormalization, no caps. */
#include "turing/ty_qcont.h"
#include "turing/ty_math.h"

#include <fenv.h>
#include <math.h>

#define TYQ_LN2   0.693147180559945309417232121458
#define TYQ_HLN2PI 0.918938533204672741780329736406 /* 0.5*ln(2*pi) */

const char *tyq_status_name(int s)
{
    switch (s) {
    case TYQ_OK: return "OK";
    case TYQ_FAIL_PROTOCOL: return "FAIL_PROTOCOL";
    case TYQ_FAIL_FLOOR: return "FAIL_FLOOR";
    case TYQ_FAIL_LEAK: return "FAIL_LEAK";
    case TYQ_FAIL_DIGEST: return "FAIL_DIGEST";
    case TYQ_FAIL_PROFILE: return "FAIL_PROFILE";
    case TYQ_FAIL_BASELINE: return "FAIL_BASELINE";
    case TYQ_E_ARG: return "E_ARG";
    case TYQ_E_IO: return "E_IO";
    default: return "UNKNOWN";
    }
}

double tyq_lnsinhc(double a)
{
    if (a < 1e-4)
        return a * a / 6.0;
    if (a < 1.0)
        return log(sinh(a) / a);
    return a - log(2.0 * a) + log1p(-exp(-2.0 * a));
}

int ty_qcont_bits(const tyq_pred *p, int64_t k, double *bits, int *floor_hit)
{
    if (!p || !bits)
        return TYQ_E_ARG;
    if (fegetround() != FE_TONEAREST)
        return TYQ_FAIL_PROTOCOL;
    if (p->family != TYQ_FAM_GAUSS)
        return TYQ_FAIL_PROTOCOL;
    if (!isfinite(p->loc) || !isfinite(p->scale) || !(p->scale > 0.0))
        return TYQ_FAIL_PROTOCOL;
    if (k >= TYQ_K_LIMIT || k <= -TYQ_K_LIMIT)
        return TYQ_FAIL_PROTOCOL;

    double s = p->scale;
    int hit = 0;
    if (s < TYQ_SD_MIN) {
        s = TYQ_SD_MIN;
        hit = 1;
    }
    double zc = (((double)k + 0.5) * TYQ_DELTA - p->loc) / s;
    double b = TYQ_DELTA / s;
    if (!isfinite(zc) || !isfinite(b) || !(b > 0.0))
        return TYQ_FAIL_PROTOCOL;
    double a = fabs(zc) * b / 2.0;
    double nll = zc * zc / 2.0 + TYQ_HLN2PI - log(b) - tyq_lnsinhc(a);
    double v = nll / TYQ_LN2;
    if (!isfinite(v))
        return TYQ_FAIL_PROTOCOL;
    *bits = v;
    if (floor_hit)
        *floor_hit = hit;
    return TYQ_OK;
}

int ty_qcont_point_ub(const tyq_pred *p, int64_t k, int64_t *ub, int *floor_hit)
{
    double bits;
    int hit = 0;
    if (!ub)
        return TYQ_E_ARG;
    int rc = ty_qcont_bits(p, k, &bits, &hit);
    if (rc != TYQ_OK)
        return rc;
    double scaled = 1e6 * bits;
    if (!isfinite(scaled) || scaled > 9.0e18 || scaled < -9.0e18)
        return TYQ_FAIL_PROTOCOL;
    *ub = (int64_t)llrint(scaled);
    if (floor_hit)
        *floor_hit = hit;
    return TYQ_OK;
}

int ty_qcont_sum_ub(const tyq_pred *p, const int64_t *k, size_t n,
                    int64_t *ld_ub, uint64_t *floor_hits)
{
    if (!ld_ub || !floor_hits || (n && (!p || !k)))
        return TYQ_E_ARG;
    int64_t sum = 0;
    uint64_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t ub;
        int hit = 0;
        int rc = ty_qcont_point_ub(&p[i], k[i], &ub, &hit);
        if (rc != TYQ_OK)
            return rc;
        if (ty_add(sum, ub, &sum) != TY_OK)
            return TYQ_FAIL_PROTOCOL;
        hits += (uint64_t)hit;
    }
    *ld_ub = sum;
    *floor_hits = hits;
    return TYQ_OK;
}

int ty_qcont_bytes_ub(uint32_t program_bytes, int64_t *bytes_ub)
{
    if (!bytes_ub)
        return TYQ_E_ARG;
    *bytes_ub = INT64_C(8000000) * (int64_t)program_bytes;
    return TYQ_OK;
}

int ty_qcont_rider_ub(uint32_t k_params, uint32_t n_prefix_incr, int64_t *rider_ub)
{
    if (!rider_ub)
        return TYQ_E_ARG;
    if (fegetround() != FE_TONEAREST)
        return TYQ_FAIL_PROTOCOL;
    if (k_params == 0) {
        *rider_ub = 0;
        return TYQ_OK;
    }
    if (n_prefix_incr == 0)
        return TYQ_FAIL_PROTOCOL;
    double v = 1e6 * ((double)k_params * 0.5 * log2((double)n_prefix_incr));
    if (!isfinite(v) || v > 9.0e18)
        return TYQ_FAIL_PROTOCOL;
    *rider_ub = (int64_t)llrint(v);
    return TYQ_OK;
}

int ty_qcont_floor_check(uint64_t floor_hits, uint64_t scored_points)
{
    if (floor_hits > UINT64_MAX / 1000u)
        return TYQ_FAIL_FLOOR;
    return floor_hits * 1000u > scored_points ? TYQ_FAIL_FLOOR : TYQ_OK;
}
