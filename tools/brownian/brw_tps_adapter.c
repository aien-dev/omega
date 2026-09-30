/* TPS1 binariser. See brw_tps_adapter.h. Requires long double with a 113-bit
 * mantissa so narrow bin masses keep relative precision. */
#include "brownian/brw_tps_adapter.h"
#include "turing/ty_qcont.h"

#include <float.h>
#include <math.h>

#if LDBL_MANT_DIG < 113
#error "brw_tps_adapter needs binary128 long double"
#endif

#define LN2L    0.693147180559945309417232121458176568L
#define SQRT1_2 0.707106781186547524400844362104849039L
#define HLN2PI  0.918938533204672741780329736406L /* 0.5 ln(2 pi) */
#define TOPCLS  51                                /* last class has no stop step */
#define MU_LIMIT 2147483648.0                     /* 2^31, generator bound */

typedef struct { long double loc, s; int64_t c; } tps_ctx;

/* log Phi(z), z <= 0 (or -inf). */
static long double tps_lphi(long double z)
{
    if (isinf(z))
        return -INFINITY;
    if (z > -35.0L)
        return logl(0.5L * erfcl(-z * SQRT1_2));
    long double z2 = z * z, term = 1.0L, sum = 1.0L, prev = 1.0L;
    for (int n = 1; n < 60; n++) {
        term *= -(long double)(2 * n - 1) / z2;
        if (fabsl(term) > prev)
            break;
        sum += term;
        prev = fabsl(term);
        if (prev < 1e-36L)
            break;
    }
    return -z2 / 2.0L - logl(-z) - HLN2PI + logl(sum);
}

/* natural-log mass of the standard Normal on (lo, hi); infinities allowed. */
static int tps_lmass(long double lo, long double hi, long double *out)
{
    if (!(lo < hi))
        return BRW_TPS_E_NUM;
    if (lo >= 0.0L) {
        long double t = lo;
        lo = -hi;
        hi = -t;
    }
    if (hi > 0.0L) {
        long double pl = expl(tps_lphi(lo)), ph = expl(tps_lphi(-hi));
        *out = log1pl(-(pl + ph));
    } else {
        long double lh = tps_lphi(hi), ll = tps_lphi(lo);
        *out = isinf(ll) ? lh : lh + logl(-expm1l(-(lh - ll)));
    }
    return (*out == *out && *out > -1e300L) ? BRW_TPS_OK : BRW_TPS_E_NUM;
}

static long double tps_edge(const tps_ctx *x, int64_t i)
{
    return ((long double)i * (long double)TYQ_DELTA - x->loc) / x->s;
}

/* mass of the bins whose |offset| lies in [a, b) (b < 0 means unbounded) on
 * side +1 (above centre) or -1 (below). a >= 1. */
static int tps_side_mass(const tps_ctx *x, int side, int64_t a, int64_t b, long double *out)
{
    long double lo, hi;
    if (side > 0) {
        lo = tps_edge(x, x->c + a);
        hi = b < 0 ? INFINITY : tps_edge(x, x->c + b);
    } else {
        lo = b < 0 ? -INFINITY : tps_edge(x, x->c - b + 1);
        hi = tps_edge(x, x->c - a + 1);
    }
    return tps_lmass(lo, hi, out);
}

static long double tps_logaddexp(long double a, long double b)
{
    if (isinf(a) && a < 0) return b;
    if (isinf(b) && b < 0) return a;
    long double m = a > b ? a : b, d = fabsl(a - b);
    return m + log1pl(expl(-d));
}

static int tps_push(brw_tps_step *st, size_t cap, size_t *n, int bit,
                    long double l1, long double l0)
{
    if (*n >= cap)
        return BRW_TPS_E_CAP;
    long double lt = bit ? l1 : l0, lo = bit ? l0 : l1;
    double p1 = (double)expl(l1);
    st[*n].p1 = p1;
    st[*n].bit = bit;
    st[*n].log2p_taken = lt / LN2L;
    st[*n].log2p_other = lo / LN2L;
    (*n)++;
    return BRW_TPS_OK;
}

int brw_tps_binarise(double mu, double sd, int64_t b, size_t cap,
                     brw_tps_step *steps, size_t *nsteps,
                     long double *bits_steps, long double *bits_exact)
{
    if (!steps || !nsteps)
        return BRW_TPS_E_ARG;
    *nsteps = 0;
    if (!isfinite(mu) || !isfinite(sd) || !(sd > 0.0) || fabs(mu) >= MU_LIMIT)
        return BRW_TPS_E_RANGE;
    /* mirror qint.v1's refusals (rounding mode, |b| limit) */
    tyq_pred p = { 0, TYQ_FAM_GAUSS, mu, sd, 0, { { 0, 0, 0 } } };
    double qb;
    if (ty_qcont_bits(&p, b, &qb, NULL) != TYQ_OK)
        return BRW_TPS_E_RANGE;

    tps_ctx x;
    x.loc = (long double)mu;
    x.s = (long double)(sd < TYQ_SD_MIN ? TYQ_SD_MIN : sd);
    x.c = (int64_t)floor(mu * 1048576.0);
    int64_t d = b - x.c, ad = d < 0 ? -d : d;
    int side = d < 0 ? -1 : 1;
    size_t n = 0;
    long double lc, lpos, lneg, lnz;
    int rc;

    if ((rc = tps_lmass(tps_edge(&x, x.c), tps_edge(&x, x.c + 1), &lc)) != 0) return rc;
    if ((rc = tps_side_mass(&x, 1, 1, -1, &lpos)) != 0) return rc;
    if ((rc = tps_side_mass(&x, -1, 1, -1, &lneg)) != 0) return rc;
    lnz = tps_logaddexp(lpos, lneg);
    /* root mass is 1 (log 0) */
    if ((rc = tps_push(steps, cap, &n, d != 0, lnz, lc)) != 0) return rc;
    if (d != 0) {
        long double lside = side > 0 ? lpos : lneg;
        if ((rc = tps_push(steps, cap, &n, side > 0, lpos - lnz, lneg - lnz)) != 0) return rc;
        int cls = 0;
        while (cls < TOPCLS && ((int64_t)1 << (cls + 1)) <= ad)
            cls++;
        long double lpar = lside;                       /* mass of |offset| >= 2^j */
        for (int j = 0; j < TOPCLS; j++) {
            long double lcl, lnext;
            int64_t a = (int64_t)1 << j;
            if ((rc = tps_side_mass(&x, side, a, a << 1, &lcl)) != 0) return rc;
            if ((rc = tps_side_mass(&x, side, a << 1, -1, &lnext)) != 0) return rc;
            int stop = (j == cls);
            if ((rc = tps_push(steps, cap, &n, stop, lcl - lpar, lnext - lpar)) != 0) return rc;
            if (stop) { lpar = lcl; break; }
            lpar = lnext;
        }
        /* refinement inside class cls, [lo, lo + 2^cls), top class unbounded above */
        int64_t lo = (int64_t)1 << cls;
        int inf = (cls == TOPCLS);
        for (int bit = cls - 1; bit >= 0; bit--) {
            int64_t half = (int64_t)1 << bit, mid = lo + half;
            int64_t hi = inf ? -1 : lo + (half << 1);
            long double lup, llow;
            if ((rc = tps_side_mass(&x, side, mid, hi, &lup)) != 0) return rc;
            if ((rc = tps_side_mass(&x, side, lo, mid, &llow)) != 0) return rc;
            int up = ad >= mid;
            if ((rc = tps_push(steps, cap, &n, up, lup - lpar, llow - lpar)) != 0) return rc;
            if (up) { lpar = lup; lo = mid; }
            else { lpar = llow; inf = 0; }
        }
    }
    *nsteps = n;
    if (bits_steps) {
        long double s = 0.0L;
        for (size_t i = 0; i < n; i++)
            s -= (long double)steps[i].log2p_taken;
        *bits_steps = s;
    }
    if (bits_exact) {
        long double le;
        if (d == 0)
            le = lc;
        else if ((rc = tps_side_mass(&x, side, ad, ad + 1, &le)) != 0)
            return rc;
        *bits_exact = -le / LN2L;
    }
    return BRW_TPS_OK;
}

int brw_tps_check_steps(const brw_tps_step *steps, size_t n)
{
    if (!steps)
        return BRW_TPS_E_ARG;
    for (size_t i = 0; i < n; i++) {
        long double l1 = steps[i].bit ? steps[i].log2p_taken : steps[i].log2p_other;
        long double l0 = steps[i].bit ? steps[i].log2p_other : steps[i].log2p_taken;
        long double q1 = exp2l(l1), q0 = exp2l(l0);
        if (!(fabsl(q1 + q0 - 1.0L) <= 1e-12))
            return BRW_TPS_E_PROOF;
        if (!(fabsl((long double)steps[i].p1 - q1) <= 1e-15 + 1e-12 * q1))
            return BRW_TPS_E_PROOF;
    }
    return BRW_TPS_OK;
}

int brw_tps_check_stream(const double *mu, const double *sd, const int64_t *b,
                         size_t n, size_t *bad_index,
                         double *max_dev_exact, double *max_dev_qint,
                         double *max_qint_excess)
{
    if (!bad_index || !max_dev_exact || !max_dev_qint || !max_qint_excess || (n && (!mu || !sd || !b)))
        return BRW_TPS_E_ARG;
    brw_tps_step st[BRW_TPS_MAX_STEPS];
    double mx = 0.0, mq = 0.0, me = -1e300;
    for (size_t i = 0; i < n; i++) {
        size_t ns;
        long double bs, be;
        double bq;
        *bad_index = i;
        int rc = brw_tps_binarise(mu[i], sd[i], b[i], BRW_TPS_MAX_STEPS, st, &ns, &bs, &be);
        if (rc != BRW_TPS_OK)
            return rc;
        tyq_pred p = { 0, TYQ_FAM_GAUSS, mu[i], sd[i], 0, { { 0, 0, 0 } } };
        if (ty_qcont_bits(&p, b[i], &bq, NULL) != TYQ_OK)
            return BRW_TPS_E_RANGE;
        if (brw_tps_check_steps(st, ns) != BRW_TPS_OK)
            return BRW_TPS_E_PROOF;
        double dx = (double)fabsl(bs - be), dq = (double)fabsl(bs - (long double)bq);
        if (!(dx <= BRW_TPS_EXACT_TOL))
            return BRW_TPS_E_PROOF;
        double r = TYQ_DELTA / (sd[i] < TYQ_SD_MIN ? TYQ_SD_MIN : sd[i]);
        double bound = -log2(1.0 - r * r / 8.0);
        if (!(dq <= bound + BRW_TPS_EXACT_TOL))
            return BRW_TPS_E_PROOF;
        if (dx > mx) mx = dx;
        if (dq > mq) mq = dq;
        if (dq - bound > me) me = dq - bound;
    }
    *max_dev_exact = mx;
    *max_dev_qint = mq;
    *max_qint_excess = me;
    return BRW_TPS_OK;
}

int brw_tps_p1_to_freq(double p1, uint16_t freq[2])
{
    if (!freq || !(p1 == p1))
        return BRW_TPS_E_ARG;
    double f = p1 * 65536.0;
    long f1 = f >= 65535.0 ? 65535 : f <= 1.0 ? 1 : lrint(f);
    if (f1 < 1) f1 = 1;
    if (f1 > 65535) f1 = 65535;
    freq[1] = (uint16_t)f1;
    freq[0] = (uint16_t)(65536 - f1);
    return BRW_TPS_OK;
}

int brw_tps_emit(const brw_tps_step *steps, size_t n, brw_tps_coder_fn fn, void *ctx)
{
    if (!steps || !fn)
        return BRW_TPS_E_ARG;
    for (size_t i = 0; i < n; i++) {
        uint16_t f[2];
        int rc = brw_tps_p1_to_freq(steps[i].p1, f);
        if (rc != BRW_TPS_OK)
            return rc;
        if (fn(ctx, f, steps[i].bit) != 0)
            return BRW_TPS_E_CODER;
    }
    return BRW_TPS_OK;
}
