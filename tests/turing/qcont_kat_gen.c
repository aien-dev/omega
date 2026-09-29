/* Independent known-answer generator for the qint.v1 scorer. Composite
 * Simpson in long double over the standardized coordinate with the peak
 * factor taken out (the raw density underflows above |z| ~ 150):
 *   u = z - zc,  I = integral_{zlo}^{zhi} exp(-u*(2*zc+u)/2) dz
 *   -ln p = zc^2/2 + 0.5*ln(2*pi) - ln(I)
 * No ln(scale) term: integrating over z already absorbs 1/scale.
 * Each row carries its own analytic Simpson error bound; a row whose relative
 * bound exceeds 1e-12 is refused. Included by test_ty_qcont.c, or built alone
 * with -DQKAT_MAIN to print the table. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define QKAT_PANELS 4096
#define QKAT_PI 3.14159265358979323846264338327950288L

typedef struct {
    double scale, loc;
    int64_t k;
    long double zc, bits, rel_bound;
} qkat_row;

/* Returns 0, or -1 if the analytic bound exceeds 1e-12 (row refused). */
static int qkat_ref(double scale, double loc, int64_t k, qkat_row *r)
{
    const long double delta = 0x1p-20L;
    long double s = scale < 0x1p-10 ? 0x1p-10L : (long double)scale;
    long double zlo = ((long double)k * delta - loc) / s;
    long double zhi = (((long double)k + 1.0L) * delta - loc) / s;
    long double zc = (((long double)k + 0.5L) * delta - loc) / s;
    long double w = zhi - zlo, h = w / QKAT_PANELS, sum = 0.0L;
    for (int i = 0; i <= QKAT_PANELS; i++) {
        long double u = -w / 2.0L + h * (long double)i;
        long double f = expl(-u * (2.0L * zc + u) / 2.0L);
        sum += f * ((i == 0 || i == QKAT_PANELS) ? 1.0L : (i & 1) ? 4.0L : 2.0L);
    }
    long double I = sum * h / 3.0L;
    /* f = exp(zc^2/2) * exp(-t^2/2), t = u + zc: f'''' = f * He4(t), and
     * f <= exp(|zc|*w/2), |He4(t)| <= t^4 + 6 t^2 + 3, |t| <= |zc| + w/2.
     * I >= w * exp(-|zc|*w/2 - w^2/8). */
    long double az = fabsl(zc), t = az + w / 2.0L;
    long double he = t * t * t * t + 6.0L * t * t + 3.0L;
    long double bound = h * h * h * h / 180.0L * expl(az * w + w * w / 8.0L) * he;
    r->scale = scale; r->loc = loc; r->k = k; r->zc = zc; r->rel_bound = bound;
    r->bits = (zc * zc / 2.0L + 0.5L * logl(2.0L * QKAT_PI) - logl(I)) / logl(2.0L);
    return bound > 1e-12L ? -1 : 0;
}

#ifdef QKAT_MAIN
int main(void)
{
    const double ss[] = {0x1p-10, 1.0, 1e3};
    const double zs[] = {0, 1, 5, 30, 2048, 4096};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 6; j++) {
            int64_t k = -12345;
            double m = ((double)k + 0.5) * 0x1p-20 - zs[j] * ss[i];
            qkat_row r;
            int rc = qkat_ref(ss[i], m, k, &r);
            printf("s=%g zc=%g bits=%.12Lf bound=%.3Le %s\n", ss[i], zs[j], r.bits,
                   r.rel_bound, rc ? "REFUSED" : "ok");
        }
    return 0;
}
#endif
